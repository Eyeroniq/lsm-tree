#include "lsm/lsm.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace lsm {
namespace {

constexpr std::uint32_t kWalMagic = 0x314C4157U;    // WAL1
constexpr std::uint32_t kTableMagic = 0x31545353U;  // SST1
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kWalHeader = 24;
constexpr std::size_t kTableHeader = 24;
constexpr std::size_t kEntryHeader = 20;
constexpr std::size_t kChecksum = 4;
constexpr std::uint32_t kMaxKey = 1U << 20;
constexpr std::uint32_t kMaxValue = 16U << 20;

[[noreturn]] void system_error(const std::string& operation) {
  throw std::runtime_error(operation + ": " + std::strerror(errno));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

std::uint32_t read_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(*data++) << shift;
  }
  return value;
}

std::uint64_t read_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(*data++) << shift;
  }
  return value;
}

void write_all(int fd, const std::uint8_t* data, std::size_t size) {
  const char* split = std::getenv("LSM_SPLIT_WRITES");
  const bool split_writes = split != nullptr && std::string(split) == "1";
  const std::size_t chunk_size = split_writes ? 7 : size;
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t requested = std::min(chunk_size, size - offset);
    const ssize_t count = ::write(fd, data + offset, requested);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      system_error("write");
    }
    if (count == 0) {
      throw std::runtime_error("write returned zero bytes");
    }
    offset += static_cast<std::size_t>(count);
    if (split_writes) {
      ::usleep(100);
    }
  }
}

bool read_exact(int fd, std::uint64_t offset, std::uint8_t* data,
                std::size_t size) {
  std::size_t complete = 0;
  while (complete < size) {
    const ssize_t count =
        ::pread(fd, data + complete, size - complete,
                static_cast<off_t>(offset + complete));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      system_error("pread");
    }
    if (count == 0) {
      return false;
    }
    complete += static_cast<std::size_t>(count);
  }
  return true;
}

std::uint64_t fnv1a(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::uint64_t mix(std::uint64_t value) {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

class Bloom {
 public:
  Bloom() = default;

  Bloom(std::size_t key_count, std::size_t bits_per_key) {
    bit_count_ = std::max<std::size_t>(64, key_count * bits_per_key);
    bytes_.assign((bit_count_ + 7) / 8, 0);
    hash_count_ = static_cast<std::uint32_t>(
        std::clamp<std::size_t>(bits_per_key * 69 / 100, 1, 12));
  }

  Bloom(std::vector<std::uint8_t> bytes, std::size_t bit_count,
        std::uint32_t hash_count)
      : bytes_(std::move(bytes)),
        bit_count_(bit_count),
        hash_count_(hash_count) {}

  void add(const std::string& key) {
    const std::uint64_t first = fnv1a(key);
    const std::uint64_t second = mix(first) | 1ULL;
    for (std::uint32_t i = 0; i < hash_count_; ++i) {
      const std::size_t bit =
          static_cast<std::size_t>((first + i * second) % bit_count_);
      bytes_[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
    }
  }

  bool maybe_contains(const std::string& key) const {
    if (bytes_.empty()) {
      return false;
    }
    const std::uint64_t first = fnv1a(key);
    const std::uint64_t second = mix(first) | 1ULL;
    for (std::uint32_t i = 0; i < hash_count_; ++i) {
      const std::size_t bit =
          static_cast<std::size_t>((first + i * second) % bit_count_);
      if ((bytes_[bit / 8] & static_cast<std::uint8_t>(1U << (bit % 8))) ==
          0) {
        return false;
      }
    }
    return true;
  }

  const std::vector<std::uint8_t>& bytes() const { return bytes_; }
  std::size_t bit_count() const { return bit_count_; }
  std::uint32_t hash_count() const { return hash_count_; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t bit_count_ = 0;
  std::uint32_t hash_count_ = 0;
};

std::uint64_t generation_from_path(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  if (name.rfind("sst-", 0) != 0 || path.extension() != ".dat") {
    return 0;
  }
  const std::string number = name.substr(4, name.size() - 8);
  if (number.empty() ||
      !std::all_of(number.begin(), number.end(),
                   [](unsigned char character) {
                     return std::isdigit(character) != 0;
                   })) {
    return 0;
  }
  return std::stoull(number);
}

}  // namespace

struct LSMTree::Entry {
  std::uint64_t sequence = 0;
  bool tombstone = false;
  std::string value;
};

struct LSMTree::Table {
  std::filesystem::path path;
  std::uint64_t generation = 0;
  Bloom bloom;
  std::vector<std::pair<std::string, Entry>> entries;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
          static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

namespace {

std::shared_ptr<LSMTree::Table> load_table(
    const std::filesystem::path& path) {
  const std::uint64_t generation = generation_from_path(path);
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    system_error("open SSTable");
  }
  try {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
      system_error("fstat SSTable");
    }
    const std::size_t size = static_cast<std::size_t>(info.st_size);
    if (size < kTableHeader + kChecksum) {
      throw std::runtime_error("SSTable is too small");
    }
    std::vector<std::uint8_t> bytes(size);
    if (!read_exact(fd, 0, bytes.data(), bytes.size())) {
      throw std::runtime_error("short SSTable read");
    }
    if (read_u32(bytes.data()) != kTableMagic ||
        read_u32(bytes.data() + 4) != kVersion) {
      throw std::runtime_error("unsupported SSTable header");
    }
    const std::uint32_t stored_crc = read_u32(bytes.data() + size - kChecksum);
    if (crc32(bytes.data(), size - kChecksum) != stored_crc) {
      throw std::runtime_error("SSTable checksum mismatch");
    }

    const std::uint64_t count = read_u64(bytes.data() + 8);
    const std::uint32_t bloom_bits = read_u32(bytes.data() + 16);
    const std::uint32_t bloom_hashes = read_u32(bytes.data() + 20);
    const std::size_t bloom_bytes = (bloom_bits + 7U) / 8U;
    if (kTableHeader + bloom_bytes + kChecksum > size) {
      throw std::runtime_error("invalid SSTable Bloom filter length");
    }
    std::vector<std::uint8_t> bloom(
        bytes.begin() + static_cast<std::ptrdiff_t>(kTableHeader),
        bytes.begin() +
            static_cast<std::ptrdiff_t>(kTableHeader + bloom_bytes));

    auto table = std::make_shared<LSMTree::Table>();
    table->path = path;
    table->generation = generation;
    table->bloom = Bloom(std::move(bloom), bloom_bits, bloom_hashes);
    std::size_t offset = kTableHeader + bloom_bytes;
    for (std::uint64_t index = 0; index < count; ++index) {
      if (offset + kEntryHeader > size - kChecksum) {
        throw std::runtime_error("truncated SSTable entry");
      }
      const std::uint64_t sequence = read_u64(bytes.data() + offset);
      const bool tombstone = bytes[offset + 8] != 0;
      if (bytes[offset + 9] != 0 || bytes[offset + 10] != 0 ||
          bytes[offset + 11] != 0) {
        throw std::runtime_error("invalid SSTable entry flags");
      }
      const std::uint32_t key_length = read_u32(bytes.data() + offset + 12);
      const std::uint32_t value_length = read_u32(bytes.data() + offset + 16);
      const std::size_t record_size =
          kEntryHeader + static_cast<std::size_t>(key_length) +
          static_cast<std::size_t>(value_length);
      if (key_length == 0 || key_length > kMaxKey ||
          value_length > kMaxValue ||
          offset + record_size > size - kChecksum) {
        throw std::runtime_error("invalid SSTable entry length");
      }
      const char* payload =
          reinterpret_cast<const char*>(bytes.data() + offset + kEntryHeader);
      std::string key(payload, key_length);
      std::string value(payload + key_length, value_length);
      if (tombstone && !value.empty()) {
        throw std::runtime_error("tombstone has a value");
      }
      if (!table->entries.empty() &&
          table->entries.back().first >= key) {
        throw std::runtime_error("SSTable keys are not strictly ordered");
      }
      table->entries.push_back(
          {std::move(key), {sequence, tombstone, std::move(value)}});
      offset += record_size;
    }
    if (offset != size - kChecksum) {
      throw std::runtime_error("unexpected bytes after SSTable entries");
    }
    ::close(fd);
    return table;
  } catch (...) {
    ::close(fd);
    throw;
  }
}

}  // namespace

LSMTree::LSMTree(std::filesystem::path directory, Options options)
    : directory_(std::move(directory)),
      wal_path_(directory_ / "active.wal"),
      options_(options) {
  if (options_.memtable_bytes == 0 || options_.bloom_bits_per_key == 0 ||
      options_.compaction_trigger < 2) {
    throw std::invalid_argument("invalid LSM options");
  }
  std::filesystem::create_directories(directory_);
  discover_tables();
  wal_fd_ = ::open(wal_path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
  if (wal_fd_ < 0) {
    system_error("open WAL");
  }
  try {
    recover_wal();
  } catch (...) {
    ::close(wal_fd_);
    wal_fd_ = -1;
    throw;
  }
}

LSMTree::~LSMTree() {
  if (wal_fd_ >= 0) {
    ::close(wal_fd_);
  }
}

void LSMTree::discover_tables() {
  tables_.clear();
  for (const auto& item : std::filesystem::directory_iterator(directory_)) {
    if (!item.is_regular_file()) {
      continue;
    }
    const std::uint64_t generation = generation_from_path(item.path());
    if (generation == 0) {
      continue;
    }
    auto table = load_table(item.path());
    tables_.push_back(std::move(table));
    next_generation_ = std::max(next_generation_, generation + 1);
  }
  std::sort(tables_.begin(), tables_.end(),
            [](const auto& left, const auto& right) {
              return left->generation > right->generation;
            });
  for (const auto& table : tables_) {
    for (const auto& [key, entry] : table->entries) {
      static_cast<void>(key);
      sequence_ = std::max(sequence_, entry.sequence);
    }
  }
}

void LSMTree::recover_wal() {
  struct stat info {};
  if (::fstat(wal_fd_, &info) != 0) {
    system_error("fstat WAL");
  }
  const std::uint64_t file_size = static_cast<std::uint64_t>(info.st_size);
  std::uint64_t offset = 0;
  while (offset < file_size) {
    if (file_size - offset < kWalHeader) {
      break;
    }
    std::vector<std::uint8_t> header(kWalHeader);
    if (!read_exact(wal_fd_, offset, header.data(), header.size())) {
      break;
    }
    const std::uint32_t magic = read_u32(header.data());
    const std::uint64_t sequence = read_u64(header.data() + 4);
    const std::uint8_t type = header[12];
    const std::uint32_t key_length = read_u32(header.data() + 16);
    const std::uint32_t value_length = read_u32(header.data() + 20);
    const std::uint64_t record_size =
        kWalHeader + static_cast<std::uint64_t>(key_length) +
        static_cast<std::uint64_t>(value_length) + kChecksum;
    if (magic != kWalMagic || sequence == 0 || (type != 1 && type != 2) ||
        header[13] != 0 || header[14] != 0 || header[15] != 0 ||
        key_length == 0 || key_length > kMaxKey || value_length > kMaxValue ||
        (type == 2 && value_length != 0) ||
        record_size > file_size - offset ||
        record_size > std::numeric_limits<std::size_t>::max()) {
      break;
    }
    std::vector<std::uint8_t> record(static_cast<std::size_t>(record_size));
    if (!read_exact(wal_fd_, offset, record.data(), record.size())) {
      break;
    }
    const std::uint32_t stored =
        read_u32(record.data() + record.size() - kChecksum);
    if (crc32(record.data(), record.size() - kChecksum) != stored) {
      break;
    }
    const char* payload =
        reinterpret_cast<const char*>(record.data() + kWalHeader);
    std::string key(payload, key_length);
    std::string value(payload + key_length, value_length);
    const Entry entry{sequence, type == 2, std::move(value)};
    const auto current = memtable_.find(key);
    if (current == memtable_.end() ||
        current->second.sequence <= entry.sequence) {
      if (current != memtable_.end()) {
        memtable_size_ -= current->first.size() + current->second.value.size();
      }
      memtable_size_ += key.size() + entry.value.size();
      memtable_[std::move(key)] = entry;
    }
    sequence_ = std::max(sequence_, sequence);
    offset += record_size;
  }
  if (offset != file_size) {
    if (::ftruncate(wal_fd_, static_cast<off_t>(offset)) != 0 ||
        ::fsync(wal_fd_) != 0) {
      system_error("repair WAL");
    }
  }
}

void LSMTree::put(const std::string& key, const std::string& value) {
  mutate(key, value, false);
}

void LSMTree::erase(const std::string& key) { mutate(key, {}, true); }

void LSMTree::mutate(const std::string& key, const std::string& value,
                     bool tombstone) {
  if (key.empty() || key.size() > kMaxKey || value.size() > kMaxValue) {
    throw std::invalid_argument("invalid key or value length");
  }
  const std::uint64_t sequence = ++sequence_;
  std::vector<std::uint8_t> record;
  record.reserve(kWalHeader + key.size() + value.size() + kChecksum);
  append_u32(record, kWalMagic);
  append_u64(record, sequence);
  record.push_back(tombstone ? 2 : 1);
  record.insert(record.end(), 3, 0);
  append_u32(record, static_cast<std::uint32_t>(key.size()));
  append_u32(record, static_cast<std::uint32_t>(value.size()));
  record.insert(record.end(), key.begin(), key.end());
  record.insert(record.end(), value.begin(), value.end());
  append_u32(record, crc32(record.data(), record.size()));
  write_all(wal_fd_, record.data(), record.size());
  if (options_.sync_writes && ::fsync(wal_fd_) != 0) {
    system_error("fsync WAL");
  }

  const auto current = memtable_.find(key);
  if (current != memtable_.end()) {
    memtable_size_ -= current->first.size() + current->second.value.size();
  }
  memtable_[key] = {sequence, tombstone, value};
  memtable_size_ += key.size() + value.size();
  if (memtable_size_ >= options_.memtable_bytes) {
    flush();
  }
}

std::optional<std::string> LSMTree::get(const std::string& key) const {
  const auto memory = memtable_.find(key);
  if (memory != memtable_.end()) {
    return memory->second.tombstone
               ? std::nullopt
               : std::optional<std::string>(memory->second.value);
  }
  for (const auto& table : tables_) {
    ++bloom_checks_;
    if (!table->bloom.maybe_contains(key)) {
      ++bloom_negative_hits_;
      continue;
    }
    const auto found = std::lower_bound(
        table->entries.begin(), table->entries.end(), key,
        [](const auto& item, const std::string& target) {
          return item.first < target;
        });
    if (found != table->entries.end() && found->first == key) {
      return found->second.tombstone
                 ? std::nullopt
                 : std::optional<std::string>(found->second.value);
    }
  }
  return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> LSMTree::scan(
    const std::string& begin, const std::string& end) const {
  if (end < begin) {
    throw std::invalid_argument("scan end precedes begin");
  }
  std::map<std::string, Entry> latest;
  for (const auto& table : tables_) {
    for (const auto& [key, entry] : table->entries) {
      if (key < begin || (!end.empty() && key >= end)) {
        continue;
      }
      const auto current = latest.find(key);
      if (current == latest.end() ||
          current->second.sequence < entry.sequence) {
        latest[key] = entry;
      }
    }
  }
  for (auto iterator = memtable_.lower_bound(begin);
       iterator != memtable_.end() &&
       (end.empty() || iterator->first < end);
       ++iterator) {
    const auto current = latest.find(iterator->first);
    if (current == latest.end() ||
        current->second.sequence < iterator->second.sequence) {
      latest[iterator->first] = iterator->second;
    }
  }
  std::vector<std::pair<std::string, std::string>> output;
  output.reserve(latest.size());
  for (const auto& [key, entry] : latest) {
    if (!entry.tombstone) {
      output.push_back({key, entry.value});
    }
  }
  return output;
}

std::shared_ptr<LSMTree::Table> LSMTree::write_table(
    const std::map<std::string, Entry>& entries) {
  Bloom bloom(entries.size(), options_.bloom_bits_per_key);
  for (const auto& [key, entry] : entries) {
    static_cast<void>(entry);
    bloom.add(key);
  }
  std::vector<std::uint8_t> bytes;
  append_u32(bytes, kTableMagic);
  append_u32(bytes, kVersion);
  append_u64(bytes, entries.size());
  append_u32(bytes, static_cast<std::uint32_t>(bloom.bit_count()));
  append_u32(bytes, bloom.hash_count());
  bytes.insert(bytes.end(), bloom.bytes().begin(), bloom.bytes().end());
  for (const auto& [key, entry] : entries) {
    append_u64(bytes, entry.sequence);
    bytes.push_back(entry.tombstone ? 1 : 0);
    bytes.insert(bytes.end(), 3, 0);
    append_u32(bytes, static_cast<std::uint32_t>(key.size()));
    append_u32(bytes, static_cast<std::uint32_t>(entry.value.size()));
    bytes.insert(bytes.end(), key.begin(), key.end());
    bytes.insert(bytes.end(), entry.value.begin(), entry.value.end());
  }
  append_u32(bytes, crc32(bytes.data(), bytes.size()));

  const std::uint64_t generation = next_generation_++;
  const std::string base = "sst-" + std::to_string(generation) + ".dat";
  const auto temporary = directory_ / (base + ".tmp");
  const auto destination = directory_ / base;
  const int fd = ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) {
    system_error("create SSTable");
  }
  try {
    write_all(fd, bytes.data(), bytes.size());
    if (::fsync(fd) != 0) {
      system_error("fsync SSTable");
    }
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
  if (::rename(temporary.c_str(), destination.c_str()) != 0) {
    system_error("rename SSTable");
  }
  sync_directory();
  return load_table(destination);
}

void LSMTree::flush() {
  if (memtable_.empty()) {
    return;
  }
  auto table = write_table(memtable_);
  tables_.insert(tables_.begin(), std::move(table));
  reset_wal();
  memtable_.clear();
  memtable_size_ = 0;
  if (tables_.size() >= options_.compaction_trigger) {
    compact();
  }
}

void LSMTree::compact() {
  if (tables_.size() < 2) {
    return;
  }
  std::map<std::string, Entry> merged;
  for (const auto& table : tables_) {
    for (const auto& [key, entry] : table->entries) {
      const auto current = merged.find(key);
      if (current == merged.end() ||
          current->second.sequence < entry.sequence) {
        merged[key] = entry;
      }
    }
  }
  auto replacement = write_table(merged);
  const auto old_tables = tables_;
  tables_.assign(1, replacement);
  for (const auto& table : old_tables) {
    std::error_code error;
    std::filesystem::remove(table->path, error);
    if (error) {
      throw std::runtime_error("failed to remove compacted SSTable");
    }
  }
  sync_directory();
}

void LSMTree::reset_wal() {
  if (::ftruncate(wal_fd_, 0) != 0 || ::fsync(wal_fd_) != 0) {
    system_error("reset WAL");
  }
}

void LSMTree::sync_directory() const {
  const int fd = ::open(directory_.c_str(), O_RDONLY);
  if (fd < 0) {
    system_error("open database directory");
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    system_error("fsync database directory");
  }
  ::close(fd);
}

Stats LSMTree::stats() const {
  return {memtable_.size(), tables_.size(), sequence_, bloom_checks_,
          bloom_negative_hits_};
}

}  // namespace lsm
