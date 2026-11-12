#include "lsm/lsm.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <list>
#include <set>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace lsm {
namespace {

constexpr std::uint32_t kWalMagic = 0x314C4157U;     // WAL1
constexpr std::uint32_t kTableMagic = 0x31545353U;   // SST1
constexpr std::uint32_t kFooterMagic = 0x46545353U;  // SSTF
// Version 1 tables are loaded whole into memory. Version 2 tables keep only
// a sparse block index and the Bloom filter in memory and read data blocks
// on demand. Both are readable; new tables are always written as version 2.
constexpr std::uint32_t kTableVersion1 = 1;
constexpr std::uint32_t kTableVersion2 = 2;
constexpr std::size_t kWalHeader = 24;
constexpr std::size_t kV1Header = 24;
constexpr std::size_t kV2Header = 8;
constexpr std::size_t kV2Footer = 64;
constexpr std::size_t kEntryHeader = 20;
constexpr std::size_t kChecksum = 4;
constexpr std::uint32_t kMaxKey = 1U << 20;
constexpr std::uint32_t kMaxValue = 16U << 20;
// The Bloom bit count is stored as a u32.
constexpr std::size_t kMaxBloomBits = 0xFFFFFFC0U;

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
    bit_count_ = std::min(kMaxBloomBits,
                          std::max<std::size_t>(64, key_count * bits_per_key));
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
    if (bytes_.empty()) {
      return;
    }
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

// Standard reflected CRC-32 (polynomial 0xEDB88320), one table lookup per
// byte instead of eight shift/xor rounds.
const std::array<std::uint32_t, 256>& crc_table() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t crc = index;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1U) != 0 ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
      }
      values[index] = crc;
    }
    return values;
  }();
  return table;
}

// One data block of a version 2 table: the last key it holds and its extent.
struct BlockHandle {
  std::string last_key;
  std::uint64_t offset = 0;
  std::uint32_t size = 0;  // Includes the trailing block CRC.
};

}  // namespace

struct LSMTree::Entry {
  std::uint64_t sequence = 0;
  bool tombstone = false;
  std::string value;
};

struct LSMTree::Table {
  std::filesystem::path path;
  std::uint64_t generation = 0;
  std::uint32_t version = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t max_sequence = 0;
  std::uint64_t file_bytes = 0;
  std::size_t level = 0;
  // Key range; both empty only for a table with no entries.
  std::string smallest;
  std::string largest;
  Bloom bloom;
  // Version 1: every entry, loaded at open.
  std::vector<std::pair<std::string, Entry>> entries;
  // Version 2: one handle per data block; blocks are read on demand.
  std::vector<BlockHandle> index;
  int fd = -1;  // Open for version 2 tables only.

  Table() = default;
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;
  ~Table() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  const auto& table = crc_table();
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
  }
  return ~crc;
}

namespace {

using Entry = LSMTree::Entry;
using Table = LSMTree::Table;
using EntryList = std::vector<std::pair<std::string, Entry>>;

}  // namespace

// LRU cache of decoded, checksum-verified data blocks, keyed by
// (table generation, block index). Generations are never reused while the
// database is open, so a cached block can never go stale; blocks of tables
// removed by compaction simply age out.
struct LSMTree::BlockCache {
  using Key = std::pair<std::uint64_t, std::size_t>;
  struct Slot {
    Key key;
    std::shared_ptr<const EntryList> block;
    std::size_t charge;
  };

  explicit BlockCache(std::size_t capacity_bytes) : capacity(capacity_bytes) {}

  std::shared_ptr<const EntryList> find(const Key& key) {
    const auto found = slots.find(key);
    if (found == slots.end()) {
      return nullptr;
    }
    lru.splice(lru.begin(), lru, found->second);
    ++hits;
    return found->second->block;
  }

  void insert(const Key& key, std::shared_ptr<const EntryList> block,
              std::size_t charge) {
    if (charge > capacity || slots.count(key) != 0) {
      return;
    }
    lru.push_front({key, std::move(block), charge});
    slots[key] = lru.begin();
    used += charge;
    while (used > capacity) {
      used -= lru.back().charge;
      slots.erase(lru.back().key);
      lru.pop_back();
    }
  }

  std::size_t capacity;
  std::size_t used = 0;
  std::uint64_t hits = 0;
  std::list<Slot> lru;  // Most recently used first.
  std::map<Key, std::list<Slot>::iterator> slots;
};

namespace {

// Entry encoding shared by both table versions:
//   [sequence u64 | tombstone u8 | 3 zero bytes | key_len u32 | value_len u32
//    | key | value]
void encode_entry(std::vector<std::uint8_t>& out, const std::string& key,
                  const Entry& entry) {
  append_u64(out, entry.sequence);
  out.push_back(entry.tombstone ? 1 : 0);
  out.insert(out.end(), 3, 0);
  append_u32(out, static_cast<std::uint32_t>(key.size()));
  append_u32(out, static_cast<std::uint32_t>(entry.value.size()));
  out.insert(out.end(), key.begin(), key.end());
  out.insert(out.end(), entry.value.begin(), entry.value.end());
}

// Decodes the entry at `offset` (which must be <= limit) and advances past it.
void decode_entry(const std::uint8_t* data, std::size_t limit,
                  std::size_t& offset, std::string& key, Entry& entry) {
  if (limit - offset < kEntryHeader) {
    throw std::runtime_error("truncated SSTable entry");
  }
  const std::uint8_t* header = data + offset;
  const std::uint8_t flag = header[8];
  if (flag > 1 || header[9] != 0 || header[10] != 0 || header[11] != 0) {
    throw std::runtime_error("invalid SSTable entry flags");
  }
  const std::uint32_t key_length = read_u32(header + 12);
  const std::uint32_t value_length = read_u32(header + 16);
  if (key_length == 0 || key_length > kMaxKey || value_length > kMaxValue ||
      limit - offset - kEntryHeader <
          static_cast<std::size_t>(key_length) + value_length) {
    throw std::runtime_error("invalid SSTable entry length");
  }
  const char* payload = reinterpret_cast<const char*>(header + kEntryHeader);
  entry.sequence = read_u64(header);
  entry.tombstone = flag == 1;
  key.assign(payload, key_length);
  entry.value.assign(payload + key_length, value_length);
  if (entry.tombstone && !entry.value.empty()) {
    throw std::runtime_error("tombstone has a value");
  }
  offset += kEntryHeader + key_length + value_length;
}

// Version 1: [header 24 | bloom | entries... | whole-file CRC32].
void load_v1(Table& table, std::size_t size) {
  if (size < kV1Header + kChecksum) {
    throw std::runtime_error("SSTable is too small");
  }
  std::vector<std::uint8_t> bytes(size);
  if (!read_exact(table.fd, 0, bytes.data(), bytes.size())) {
    throw std::runtime_error("short SSTable read");
  }
  const std::size_t limit = size - kChecksum;
  if (crc32(bytes.data(), limit) != read_u32(bytes.data() + limit)) {
    throw std::runtime_error("SSTable checksum mismatch");
  }
  const std::uint64_t count = read_u64(bytes.data() + 8);
  const std::uint32_t bloom_bits = read_u32(bytes.data() + 16);
  const std::uint32_t bloom_hashes = read_u32(bytes.data() + 20);
  const std::size_t bloom_bytes =
      static_cast<std::size_t>((static_cast<std::uint64_t>(bloom_bits) + 7) / 8);
  if (bloom_bytes > limit - kV1Header) {
    throw std::runtime_error("invalid SSTable Bloom filter length");
  }
  const auto bloom_begin = bytes.begin() + static_cast<std::ptrdiff_t>(kV1Header);
  table.bloom = Bloom(
      std::vector<std::uint8_t>(
          bloom_begin, bloom_begin + static_cast<std::ptrdiff_t>(bloom_bytes)),
      bloom_bits, bloom_hashes);
  std::size_t offset = kV1Header + bloom_bytes;
  for (std::uint64_t index = 0; index < count; ++index) {
    std::string key;
    Entry entry;
    decode_entry(bytes.data(), limit, offset, key, entry);
    if (!table.entries.empty() && table.entries.back().first >= key) {
      throw std::runtime_error("SSTable keys are not strictly ordered");
    }
    table.max_sequence = std::max(table.max_sequence, entry.sequence);
    table.entries.emplace_back(std::move(key), std::move(entry));
  }
  if (offset != limit) {
    throw std::runtime_error("unexpected bytes after SSTable entries");
  }
  table.entry_count = table.entries.size();
}

// Version 2:
//   [magic u32 | version u32]
//   data blocks:  [entries... | block CRC32]...
//   index:        [key_len u32 | last_key | offset u64 | size u32]... | CRC32
//   Bloom filter bytes
//   footer (64):  [index_offset u64 | index_size u64 | bloom_offset u64 |
//                  bloom_bytes u64 | entry_count u64 | max_sequence u64 |
//                  bloom_bits u32 | bloom_hashes u32 | magic u32 | CRC32]
// Opening reads and verifies only the footer, index and Bloom filter. Data
// blocks are verified when they are read.
void load_v2(Table& table, std::size_t size) {
  if (size < kV2Header + kV2Footer) {
    throw std::runtime_error("SSTable is too small");
  }
  const std::uint64_t data_end = size - kV2Footer;
  std::uint8_t footer[kV2Footer];
  if (!read_exact(table.fd, data_end, footer, kV2Footer)) {
    throw std::runtime_error("short SSTable footer read");
  }
  if (crc32(footer, kV2Footer - kChecksum) !=
      read_u32(footer + kV2Footer - kChecksum)) {
    throw std::runtime_error("SSTable footer checksum mismatch");
  }
  if (read_u32(footer + 56) != kFooterMagic) {
    throw std::runtime_error("invalid SSTable footer");
  }
  const std::uint64_t index_offset = read_u64(footer);
  const std::uint64_t index_size = read_u64(footer + 8);
  const std::uint64_t bloom_offset = read_u64(footer + 16);
  const std::uint64_t bloom_bytes = read_u64(footer + 24);
  const std::uint64_t entry_count = read_u64(footer + 32);
  const std::uint64_t max_sequence = read_u64(footer + 40);
  const std::uint32_t bloom_bits = read_u32(footer + 48);
  const std::uint32_t bloom_hashes = read_u32(footer + 52);
  if (index_offset < kV2Header || index_offset > data_end ||
      index_size < kChecksum || index_size > data_end - index_offset ||
      bloom_offset != index_offset + index_size ||
      bloom_bytes != data_end - bloom_offset || bloom_bits == 0 ||
      bloom_bytes != (static_cast<std::uint64_t>(bloom_bits) + 7) / 8 ||
      bloom_hashes == 0 || bloom_hashes > 32) {
    throw std::runtime_error("invalid SSTable layout");
  }

  std::vector<std::uint8_t> index(static_cast<std::size_t>(index_size));
  if (!read_exact(table.fd, index_offset, index.data(), index.size())) {
    throw std::runtime_error("short SSTable index read");
  }
  const std::size_t limit = index.size() - kChecksum;
  if (crc32(index.data(), limit) != read_u32(index.data() + limit)) {
    throw std::runtime_error("SSTable index checksum mismatch");
  }
  std::uint64_t expected_offset = kV2Header;
  std::size_t position = 0;
  while (position < limit) {
    if (limit - position < 4) {
      throw std::runtime_error("invalid SSTable index entry");
    }
    const std::uint32_t key_length = read_u32(index.data() + position);
    position += 4;
    if (key_length == 0 || key_length > kMaxKey ||
        limit - position < static_cast<std::size_t>(key_length) + 12) {
      throw std::runtime_error("invalid SSTable index entry");
    }
    BlockHandle handle;
    handle.last_key.assign(
        reinterpret_cast<const char*>(index.data() + position), key_length);
    position += key_length;
    handle.offset = read_u64(index.data() + position);
    handle.size = read_u32(index.data() + position + 8);
    position += 12;
    if (handle.offset != expected_offset ||
        handle.size < kEntryHeader + 1 + kChecksum ||
        handle.size > index_offset - handle.offset) {
      throw std::runtime_error("invalid SSTable block extent");
    }
    if (!table.index.empty() && table.index.back().last_key >= handle.last_key) {
      throw std::runtime_error("SSTable index keys are not strictly ordered");
    }
    expected_offset += handle.size;
    table.index.push_back(std::move(handle));
  }
  if (expected_offset != index_offset ||
      (entry_count == 0) != table.index.empty()) {
    throw std::runtime_error("SSTable blocks do not match the index");
  }

  std::vector<std::uint8_t> bloom(static_cast<std::size_t>(bloom_bytes));
  if (!read_exact(table.fd, bloom_offset, bloom.data(), bloom.size())) {
    throw std::runtime_error("short SSTable Bloom filter read");
  }
  table.bloom = Bloom(std::move(bloom), bloom_bits, bloom_hashes);
  table.entry_count = entry_count;
  table.max_sequence = max_sequence;
}

std::shared_ptr<Table> load_table(const std::filesystem::path& path) {
  auto table = std::make_shared<Table>();
  table->path = path;
  table->generation = generation_from_path(path);
  table->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (table->fd < 0) {
    system_error("open SSTable");
  }
  struct stat info {};
  if (::fstat(table->fd, &info) != 0) {
    system_error("fstat SSTable");
  }
  const std::size_t size = static_cast<std::size_t>(info.st_size);
  std::uint8_t header[kV2Header];
  if (size < kV2Header || !read_exact(table->fd, 0, header, kV2Header) ||
      read_u32(header) != kTableMagic) {
    throw std::runtime_error("unsupported SSTable header");
  }
  table->version = read_u32(header + 4);
  table->file_bytes = size;
  if (table->version == kTableVersion1) {
    load_v1(*table, size);
    ::close(table->fd);  // Everything is in memory now.
    table->fd = -1;
    if (!table->entries.empty()) {
      table->smallest = table->entries.front().first;
      table->largest = table->entries.back().first;
    }
  } else if (table->version == kTableVersion2) {
    load_v2(*table, size);
    if (!table->index.empty()) {
      // The smallest key needs a block read; the caller supplies it, from
      // the MANIFEST or from the first block.
      table->largest = table->index.back().last_key;
    }
  } else {
    throw std::runtime_error("unsupported SSTable version");
  }
  return table;
}

// Reads one data block of a version 2 table and verifies its checksum and
// its agreement with the index.
EntryList read_block(const Table& table, std::size_t block) {
  const BlockHandle& handle = table.index[block];
  std::vector<std::uint8_t> bytes(handle.size);
  if (!read_exact(table.fd, handle.offset, bytes.data(), bytes.size())) {
    throw std::runtime_error("short SSTable block read");
  }
  const std::size_t limit = bytes.size() - kChecksum;
  if (crc32(bytes.data(), limit) != read_u32(bytes.data() + limit)) {
    throw std::runtime_error("SSTable block checksum mismatch in " +
                             table.path.filename().string());
  }
  EntryList entries;
  std::size_t offset = 0;
  while (offset < limit) {
    std::string key;
    Entry entry;
    decode_entry(bytes.data(), limit, offset, key, entry);
    if (!entries.empty() && entries.back().first >= key) {
      throw std::runtime_error("SSTable keys are not strictly ordered");
    }
    entries.emplace_back(std::move(key), std::move(entry));
  }
  if (entries.empty() || entries.back().first != handle.last_key ||
      (block > 0 && entries.front().first <= table.index[block - 1].last_key)) {
    throw std::runtime_error("SSTable block does not match its index entry");
  }
  return entries;
}

// Ordered iteration over one table, for either format. Holds at most one
// data block of a version 2 table. With a null cache every block comes from
// disk and nothing is cached (compaction uses this to avoid evicting the
// blocks that serve reads).
class TableCursor {
 public:
  TableCursor(const Table& table, LSMTree::BlockCache* cache,
              std::uint64_t& block_reads)
      : table_(table), cache_(cache), block_reads_(block_reads) {}

  // Positions at the first key >= target.
  void seek(const std::string& target) {
    if (table_.version == kTableVersion1) {
      position_ = static_cast<std::size_t>(
          std::lower_bound(table_.entries.begin(), table_.entries.end(), target,
                           [](const auto& item, const std::string& key) {
                             return item.first < key;
                           }) -
          table_.entries.begin());
      return;
    }
    block_ = static_cast<std::size_t>(
        std::lower_bound(table_.index.begin(), table_.index.end(), target,
                         [](const BlockHandle& handle, const std::string& key) {
                           return handle.last_key < key;
                         }) -
        table_.index.begin());
    position_ = 0;
    block_entries_.reset();
    if (block_ == table_.index.size()) {
      return;
    }
    load_block();
    // The block's last key is >= target, so this lands inside the block.
    position_ = static_cast<std::size_t>(
        std::lower_bound(block_entries_->begin(), block_entries_->end(), target,
                         [](const auto& item, const std::string& key) {
                           return item.first < key;
                         }) -
        block_entries_->begin());
  }

  bool valid() const {
    if (table_.version == kTableVersion1) {
      return position_ < table_.entries.size();
    }
    return block_entries_ != nullptr && position_ < block_entries_->size();
  }

  const std::string& key() const { return current().first; }
  const Entry& entry() const { return current().second; }

  void next() {
    ++position_;
    if (table_.version == kTableVersion2 &&
        position_ == block_entries_->size()) {
      ++block_;
      position_ = 0;
      block_entries_.reset();
      if (block_ < table_.index.size()) {
        load_block();
      }
    }
  }

 private:
  const std::pair<std::string, Entry>& current() const {
    return table_.version == kTableVersion1 ? table_.entries[position_]
                                            : (*block_entries_)[position_];
  }

  void load_block() {
    const LSMTree::BlockCache::Key key{table_.generation, block_};
    if (cache_ != nullptr) {
      block_entries_ = cache_->find(key);
      if (block_entries_ != nullptr) {
        return;
      }
    }
    auto loaded = std::make_shared<const EntryList>(read_block(table_, block_));
    ++block_reads_;
    if (cache_ != nullptr) {
      // Charge the on-disk size plus a rough per-entry allocation overhead.
      cache_->insert(key, loaded,
                     table_.index[block_].size + loaded->size() * 64);
    }
    block_entries_ = std::move(loaded);
  }

  const Table& table_;
  LSMTree::BlockCache* cache_;
  std::uint64_t& block_reads_;
  std::size_t block_ = 0;
  std::size_t position_ = 0;
  std::shared_ptr<const EntryList> block_entries_;
};

// Streams sorted entries into a new version 2 table. Memory use is one data
// block plus the index and Bloom filter, regardless of table size.
class TableWriter {
 public:
  TableWriter(const std::filesystem::path& path, std::size_t block_bytes,
              std::size_t bloom_bits_per_key, std::size_t expected_keys)
      : block_bytes_(block_bytes),
        bloom_(std::max<std::size_t>(expected_keys, 1), bloom_bits_per_key) {
    fd_ = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd_ < 0) {
      system_error("create SSTable");
    }
    std::vector<std::uint8_t> header;
    append_u32(header, kTableMagic);
    append_u32(header, kTableVersion2);
    write(header);
  }

  TableWriter(const TableWriter&) = delete;
  TableWriter& operator=(const TableWriter&) = delete;

  ~TableWriter() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  // Keys must arrive in strictly increasing order.
  void add(const std::string& key, const Entry& entry) {
    encode_entry(block_, key, entry);
    last_key_ = key;
    bloom_.add(key);
    max_sequence_ = std::max(max_sequence_, entry.sequence);
    ++count_;
    if (block_.size() >= block_bytes_) {
      flush_block();
    }
  }

  // Bytes written so far; after finish(), the size of the file.
  std::uint64_t bytes_written() const { return offset_; }

  // Writes the index, Bloom filter and footer, then fsyncs and closes.
  void finish() {
    flush_block();
    const std::uint64_t index_offset = offset_;
    append_u32(index_, crc32(index_.data(), index_.size()));
    write(index_);
    const std::uint64_t bloom_offset = offset_;
    write(bloom_.bytes());
    std::vector<std::uint8_t> footer;
    append_u64(footer, index_offset);
    append_u64(footer, bloom_offset - index_offset);
    append_u64(footer, bloom_offset);
    append_u64(footer, bloom_.bytes().size());
    append_u64(footer, count_);
    append_u64(footer, max_sequence_);
    append_u32(footer, static_cast<std::uint32_t>(bloom_.bit_count()));
    append_u32(footer, bloom_.hash_count());
    append_u32(footer, kFooterMagic);
    append_u32(footer, crc32(footer.data(), footer.size()));
    write(footer);
    if (::fsync(fd_) != 0) {
      system_error("fsync SSTable");
    }
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) {
      system_error("close SSTable");
    }
  }

 private:
  void flush_block() {
    if (block_.empty()) {
      return;
    }
    append_u32(block_, crc32(block_.data(), block_.size()));
    append_u32(index_, static_cast<std::uint32_t>(last_key_.size()));
    index_.insert(index_.end(), last_key_.begin(), last_key_.end());
    append_u64(index_, offset_);
    append_u32(index_, static_cast<std::uint32_t>(block_.size()));
    write(block_);
    block_.clear();
  }

  void write(const std::vector<std::uint8_t>& bytes) {
    write_all(fd_, bytes.data(), bytes.size());
    offset_ += bytes.size();
  }

  std::size_t block_bytes_;
  Bloom bloom_;
  int fd_ = -1;
  std::uint64_t offset_ = 0;
  std::vector<std::uint8_t> block_;
  std::vector<std::uint8_t> index_;
  std::string last_key_;
  std::uint64_t count_ = 0;
  std::uint64_t max_sequence_ = 0;
};

// Builds a table at `path`, removing the partial file if anything fails.
template <typename Fill>
void build_table(const std::filesystem::path& path, const Options& options,
                 std::size_t expected_keys, Fill&& fill) {
  try {
    TableWriter writer(path, options.block_bytes, options.bloom_bits_per_key,
                       expected_keys);
    fill(writer);
    writer.finish();
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    throw;
  }
}

// MANIFEST version 2 is a log: [magic | version] then records
//   [payload_len u32 | type u8 | payload | CRC32 of type + payload]
// The first record is always a full snapshot; later records are edits.
//   snapshot payload: [next_generation u64 | count u32 | entry...]
//   edit payload:     [next_generation u64 | removed u32 | generation u64... |
//                      added u32 | entry...]
//   entry:            [generation u64 | level u32 | smallest_len u32 |
//                      smallest | largest_len u32 | largest]
// A commit appends one edit and fsyncs. When the log has grown well past its
// snapshot it is rewritten as a single snapshot (tmp, fsync, rename, fsync
// directory). Version 1 (MAN1) was one snapshot with a trailing CRC; it is
// read and rewritten as version 2.
constexpr std::uint32_t kManifestMagicV1 = 0x314E414DU;  // MAN1
constexpr std::uint32_t kManifestMagic = 0x324E414DU;    // MAN2
constexpr std::uint32_t kManifestVersion = 2;
constexpr std::size_t kManifestHeader = 8;
constexpr std::size_t kManifestRecordHeader = 5;
constexpr std::uint8_t kManifestSnapshot = 1;
constexpr std::uint8_t kManifestEdit = 2;
constexpr std::uint64_t kManifestRewriteBytes = 64 * 1024;
constexpr std::size_t kMaxLevels = 7;

struct ManifestTable {
  std::uint64_t generation = 0;
  std::uint32_t level = 0;
  std::string smallest;
  std::string largest;
};

struct Manifest {
  std::uint64_t next_generation = 1;
  std::map<std::uint64_t, ManifestTable> tables;
  // Old format or an uncommitted (torn) final edit: rewrite on open.
  bool needs_rewrite = false;
  std::uint64_t snapshot_bytes = 0;
};

// Bounds-checked reader over one MANIFEST payload.
struct ManifestReader {
  const std::uint8_t* data;
  std::size_t size;
  std::size_t position = 0;

  void need(std::size_t count) const {
    if (size - position < count) {
      throw std::runtime_error("truncated MANIFEST record");
    }
  }
  std::uint32_t u32() {
    need(4);
    const std::uint32_t value = read_u32(data + position);
    position += 4;
    return value;
  }
  std::uint64_t u64() {
    need(8);
    const std::uint64_t value = read_u64(data + position);
    position += 8;
    return value;
  }
  std::string key() {
    const std::uint32_t length = u32();
    if (length > kMaxKey) {
      throw std::runtime_error("invalid MANIFEST key length");
    }
    need(length);
    std::string value(reinterpret_cast<const char*>(data + position), length);
    position += length;
    return value;
  }
  ManifestTable entry() {
    ManifestTable table;
    table.generation = u64();
    table.level = u32();
    table.smallest = key();
    table.largest = key();
    if (table.generation == 0 || table.level >= kMaxLevels) {
      throw std::runtime_error("invalid MANIFEST table entry");
    }
    return table;
  }
};

void append_manifest_entry(std::vector<std::uint8_t>& out,
                           std::uint64_t generation, std::size_t level,
                           const std::string& smallest,
                           const std::string& largest) {
  append_u64(out, generation);
  append_u32(out, static_cast<std::uint32_t>(level));
  append_u32(out, static_cast<std::uint32_t>(smallest.size()));
  out.insert(out.end(), smallest.begin(), smallest.end());
  append_u32(out, static_cast<std::uint32_t>(largest.size()));
  out.insert(out.end(), largest.begin(), largest.end());
}

std::vector<std::uint8_t> manifest_record(std::uint8_t type,
                                          const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> record;
  append_u32(record, static_cast<std::uint32_t>(payload.size()));
  record.push_back(type);
  record.insert(record.end(), payload.begin(), payload.end());
  append_u32(record, crc32(record.data() + 4, 1 + payload.size()));
  return record;
}

Manifest read_manifest_v1(const std::vector<std::uint8_t>& bytes) {
  const std::size_t limit = bytes.size() - kChecksum;
  if (crc32(bytes.data(), limit) != read_u32(bytes.data() + limit)) {
    throw std::runtime_error("MANIFEST checksum mismatch");
  }
  if (read_u32(bytes.data() + 4) != 1) {
    throw std::runtime_error("unsupported MANIFEST header");
  }
  ManifestReader reader{bytes.data(), limit, 8};
  Manifest manifest;
  manifest.next_generation = reader.u64();
  const std::uint32_t count = reader.u32();
  for (std::uint32_t index = 0; index < count; ++index) {
    ManifestTable table = reader.entry();
    if (!manifest.tables.emplace(table.generation, table).second) {
      throw std::runtime_error("MANIFEST lists a table twice");
    }
  }
  if (reader.position != limit) {
    throw std::runtime_error("unexpected bytes after MANIFEST entries");
  }
  manifest.needs_rewrite = true;
  return manifest;
}

// Returns nullopt when the file does not exist, which means the directory
// predates levels: every table in it is then treated as level 0.
std::optional<Manifest> read_manifest(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    system_error("open MANIFEST");
  }
  std::vector<std::uint8_t> bytes;
  try {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
      system_error("fstat MANIFEST");
    }
    bytes.resize(static_cast<std::size_t>(info.st_size));
    if (!read_exact(fd, 0, bytes.data(), bytes.size())) {
      throw std::runtime_error("short MANIFEST read");
    }
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);

  if (bytes.size() < kManifestHeader + kChecksum) {
    throw std::runtime_error("MANIFEST is too small");
  }
  if (read_u32(bytes.data()) == kManifestMagicV1) {
    return read_manifest_v1(bytes);
  }
  if (read_u32(bytes.data()) != kManifestMagic ||
      read_u32(bytes.data() + 4) != kManifestVersion) {
    throw std::runtime_error("unsupported MANIFEST header");
  }

  Manifest manifest;
  const std::size_t size = bytes.size();
  std::size_t position = kManifestHeader;
  bool first = true;
  while (position < size) {
    // An append cut short by a crash leaves an incomplete final record: that
    // edit was never committed. The first record was written whole (tmp file,
    // rename), so damage there is corruption.
    if (size - position < kManifestRecordHeader + kChecksum ||
        read_u32(bytes.data() + position) >
            size - position - kManifestRecordHeader - kChecksum) {
      if (first) {
        throw std::runtime_error("MANIFEST snapshot is truncated");
      }
      manifest.needs_rewrite = true;
      break;
    }
    const std::uint32_t length = read_u32(bytes.data() + position);
    const std::uint8_t type = bytes[position + 4];
    const std::size_t end = position + kManifestRecordHeader + length + kChecksum;
    if (crc32(bytes.data() + position + 4, 1 + length) !=
        read_u32(bytes.data() + end - kChecksum)) {
      // Only the very last record may be a torn append. A bad record with
      // valid records after it means the file is damaged, and trusting a
      // prefix could treat newer live tables as orphans and delete them.
      if (first || end != size) {
        throw std::runtime_error("MANIFEST is corrupt");
      }
      manifest.needs_rewrite = true;
      break;
    }
    ManifestReader reader{bytes.data() + position + kManifestRecordHeader,
                          length};
    const std::uint64_t next_generation = reader.u64();
    if (type == kManifestSnapshot) {
      manifest.tables.clear();
      const std::uint32_t count = reader.u32();
      for (std::uint32_t index = 0; index < count; ++index) {
        ManifestTable table = reader.entry();
        if (!manifest.tables.emplace(table.generation, table).second) {
          throw std::runtime_error("MANIFEST lists a table twice");
        }
      }
      manifest.snapshot_bytes = end;
    } else if (type == kManifestEdit && !first) {
      const std::uint32_t removed = reader.u32();
      for (std::uint32_t index = 0; index < removed; ++index) {
        if (manifest.tables.erase(reader.u64()) == 0) {
          throw std::runtime_error("MANIFEST edit removes an unknown table");
        }
      }
      const std::uint32_t added = reader.u32();
      for (std::uint32_t index = 0; index < added; ++index) {
        ManifestTable table = reader.entry();
        if (!manifest.tables.emplace(table.generation, table).second) {
          throw std::runtime_error("MANIFEST edit adds a table twice");
        }
      }
    } else {
      throw std::runtime_error("invalid MANIFEST record type");
    }
    if (reader.position != length) {
      throw std::runtime_error("unexpected bytes in MANIFEST record");
    }
    manifest.next_generation = next_generation;
    first = false;
    position = end;
  }
  if (first) {
    throw std::runtime_error("MANIFEST has no snapshot");
  }
  return manifest;
}

// Smallest key of a table, reading its first block if necessary.
std::string first_key(const Table& table) {
  std::uint64_t ignored = 0;
  TableCursor cursor(table, nullptr, ignored);
  cursor.seek("");
  return cursor.valid() ? cursor.key() : std::string();
}

bool overlaps(const Table& table, const std::string& smallest,
              const std::string& largest) {
  return table.entry_count != 0 && !(table.largest < smallest) &&
         !(largest < table.smallest);
}

}  // namespace

LSMTree::LSMTree(std::filesystem::path directory, Options options)
    : directory_(std::move(directory)),
      wal_path_(directory_ / "active.wal"),
      options_(options) {
  if (options_.memtable_bytes == 0 || options_.bloom_bits_per_key == 0 ||
      options_.compaction_trigger < 2 || options_.block_bytes == 0 ||
      options_.table_bytes == 0 || options_.level1_bytes == 0 ||
      options_.level_multiplier < 2) {
    throw std::invalid_argument("invalid LSM options");
  }
  block_cache_ = std::make_unique<BlockCache>(options_.block_cache_bytes);
  std::filesystem::create_directories(directory_);
  acquire_lock();
  try {
    open_tables();
    wal_fd_ = ::open(wal_path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if (wal_fd_ < 0) {
      system_error("open WAL");
    }
    // A newly created WAL's name is not durable until its directory is
    // fsynced; without this, every write acknowledged before the first flush
    // could vanish in a power cut even though the WAL itself was fsynced.
    sync_directory();
    recover_wal();
  } catch (...) {
    if (wal_fd_ >= 0) {
      ::close(wal_fd_);
      wal_fd_ = -1;
    }
    if (manifest_fd_ >= 0) {
      ::close(manifest_fd_);
      manifest_fd_ = -1;
    }
    ::close(lock_fd_);
    lock_fd_ = -1;
    throw;
  }
}

LSMTree::~LSMTree() {
  if (wal_fd_ >= 0) {
    ::close(wal_fd_);
  }
  if (manifest_fd_ >= 0) {
    ::close(manifest_fd_);
  }
  if (lock_fd_ >= 0) {
    ::close(lock_fd_);  // Releases the flock.
  }
}

void LSMTree::acquire_lock() {
  const auto lock_path = directory_ / "LOCK";
  lock_fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (lock_fd_ < 0) {
    system_error("open LOCK");
  }
  // flock locks belong to the open file description, so a second open of
  // the same directory fails in this process as well as in others.
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    const int saved = errno;
    ::close(lock_fd_);
    lock_fd_ = -1;
    if (saved == EWOULDBLOCK) {
      throw std::runtime_error("database directory is already open: " +
                               directory_.string());
    }
    errno = saved;
    system_error("lock database directory");
  }
}

void LSMTree::open_tables() {
  levels_.assign(kMaxLevels, {});
  compact_pointer_.assign(kMaxLevels, {});
  std::map<std::uint64_t, std::filesystem::path> files;
  std::vector<std::filesystem::path> abandoned;
  for (const auto& item : std::filesystem::directory_iterator(directory_)) {
    if (!item.is_regular_file()) {
      continue;
    }
    const std::string name = item.path().filename().string();
    if ((name.rfind("sst-", 0) == 0 && item.path().extension() == ".tmp") ||
        name == "MANIFEST.tmp") {
      // Never renamed into place, so never part of the database.
      abandoned.push_back(item.path());
      continue;
    }
    const std::uint64_t generation = generation_from_path(item.path());
    if (generation == 0) {
      continue;
    }
    files[generation] = item.path();
    next_generation_ = std::max(next_generation_, generation + 1);
  }
  for (const auto& path : abandoned) {
    std::error_code error;
    std::filesystem::remove(path, error);
  }

  const auto manifest = read_manifest(directory_ / "MANIFEST");
  if (manifest) {
    // The MANIFEST alone decides which tables are live.
    next_generation_ = std::max(next_generation_, manifest->next_generation);
    std::set<std::uint64_t> live;
    for (const auto& [listed, entry] : manifest->tables) {
      const auto file = files.find(entry.generation);
      if (file == files.end()) {
        throw std::runtime_error("MANIFEST references missing table sst-" +
                                 std::to_string(entry.generation) + ".dat");
      }
      if (!live.insert(entry.generation).second) {
        throw std::runtime_error("MANIFEST lists a table twice");
      }
      auto table = load_table(file->second);
      if (table->largest != entry.largest) {
        throw std::runtime_error("MANIFEST disagrees with " +
                                 file->second.filename().string());
      }
      table->level = entry.level;
      table->smallest = entry.smallest;
      levels_[entry.level].push_back(std::move(table));
    }
    // Tables missing from the MANIFEST were written by a flush or compaction
    // that crashed before committing. Their data is still in the WAL or in
    // the tables the MANIFEST lists, so they are safe to delete.
    bool removed = false;
    for (const auto& [generation, path] : files) {
      if (live.count(generation) == 0) {
        std::error_code error;
        removed = std::filesystem::remove(path, error) || removed;
      }
    }
    if (removed) {
      sync_directory();
    }
  } else {
    // A directory from before the MANIFEST existed: every table is level 0,
    // ordered by generation exactly as before.
    for (const auto& [generation, path] : files) {
      auto table = load_table(path);
      table->smallest = first_key(*table);
      levels_[0].push_back(std::move(table));
    }
  }

  std::sort(levels_[0].begin(), levels_[0].end(),
            [](const auto& left, const auto& right) {
              return left->generation > right->generation;
            });
  for (std::size_t level = 1; level < levels_.size(); ++level) {
    auto& tables = levels_[level];
    std::sort(tables.begin(), tables.end(),
              [](const auto& left, const auto& right) {
                return left->smallest < right->smallest;
              });
    for (std::size_t index = 1; index < tables.size(); ++index) {
      if (!(tables[index - 1]->largest < tables[index]->smallest)) {
        throw std::runtime_error("overlapping tables in level " +
                                 std::to_string(level));
      }
    }
  }
  for (const auto& tables : levels_) {
    for (const auto& table : tables) {
      sequence_ = std::max(sequence_, table->max_sequence);
    }
  }
  if (!manifest || manifest->needs_rewrite) {
    rewrite_manifest();
  } else {
    const auto path = directory_ / "MANIFEST";
    manifest_fd_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (manifest_fd_ < 0) {
      system_error("open MANIFEST");
    }
    manifest_bytes_ = std::filesystem::file_size(path);
    manifest_snapshot_bytes_ = manifest->snapshot_bytes;
  }
}

// Writes the current levels as a fresh single-snapshot MANIFEST: write
// MANIFEST.tmp, fsync, rename, fsync the directory. A failure before the
// rename leaves the old MANIFEST in force; a failure after it leaves the
// outcome unknown, so writes are refused until the database is reopened.
void LSMTree::rewrite_manifest() {
  std::vector<std::uint8_t> payload;
  append_u64(payload, next_generation_);
  std::uint32_t count = 0;
  for (const auto& tables : levels_) {
    count += static_cast<std::uint32_t>(tables.size());
  }
  append_u32(payload, count);
  for (std::size_t level = 0; level < levels_.size(); ++level) {
    for (const auto& table : levels_[level]) {
      append_manifest_entry(payload, table->generation, level, table->smallest,
                            table->largest);
    }
  }
  std::vector<std::uint8_t> bytes;
  append_u32(bytes, kManifestMagic);
  append_u32(bytes, kManifestVersion);
  const auto record = manifest_record(kManifestSnapshot, payload);
  bytes.insert(bytes.end(), record.begin(), record.end());

  const auto temporary = directory_ / "MANIFEST.tmp";
  const auto manifest = directory_ / "MANIFEST";
  const auto fail = [&](const char* operation) {
    const int saved = errno;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    errno = saved;
    system_error(operation);
  };
  const int fd =
      ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) {
    fail("create MANIFEST");
  }
  try {
    write_all(fd, bytes.data(), bytes.size());
  } catch (...) {
    ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    fail("fsync MANIFEST");
  }
  if (::close(fd) != 0) {
    fail("close MANIFEST");
  }
  if (::rename(temporary.c_str(), manifest.c_str()) != 0) {
    fail("rename MANIFEST");
  }
  try {
    sync_directory();
    if (manifest_fd_ >= 0) {
      ::close(manifest_fd_);
    }
    manifest_fd_ = ::open(manifest.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (manifest_fd_ < 0) {
      system_error("open MANIFEST");
    }
  } catch (...) {
    failed_ = true;
    throw;
  }
  manifest_bytes_ = bytes.size();
  manifest_snapshot_bytes_ = bytes.size();
}

// Makes `next` the live set of tables by appending one edit record (tables
// removed, tables added or moved to another level) and fsyncing it. The edit
// is the commit point: before its fsync the old tables are live, after it the
// new ones are.
void LSMTree::commit(Levels next) {
  std::map<std::uint64_t, std::size_t> before;
  for (std::size_t level = 0; level < levels_.size(); ++level) {
    for (const auto& table : levels_[level]) {
      before[table->generation] = level;
    }
  }
  std::map<std::uint64_t, std::size_t> after;
  for (std::size_t level = 0; level < next.size(); ++level) {
    for (const auto& table : next[level]) {
      after[table->generation] = level;
    }
  }
  std::vector<std::uint8_t> removed;
  std::uint32_t removed_count = 0;
  for (const auto& [generation, level] : before) {
    const auto found = after.find(generation);
    if (found == after.end() || found->second != level) {
      append_u64(removed, generation);
      ++removed_count;
    }
  }
  std::vector<std::uint8_t> added;
  std::uint32_t added_count = 0;
  for (std::size_t level = 0; level < next.size(); ++level) {
    for (const auto& table : next[level]) {
      const auto found = before.find(table->generation);
      if (found == before.end() || found->second != level) {
        append_manifest_entry(added, table->generation, level, table->smallest,
                              table->largest);
        ++added_count;
      }
    }
  }
  std::vector<std::uint8_t> payload;
  append_u64(payload, next_generation_);
  append_u32(payload, removed_count);
  payload.insert(payload.end(), removed.begin(), removed.end());
  append_u32(payload, added_count);
  payload.insert(payload.end(), added.begin(), added.end());
  const auto record = manifest_record(kManifestEdit, payload);

  try {
    write_all(manifest_fd_, record.data(), record.size());
  } catch (...) {
    // As with the WAL: a partial record must not stay in the log, or every
    // later edit would sit behind an unreadable one.
    if (::ftruncate(manifest_fd_, static_cast<off_t>(manifest_bytes_)) != 0) {
      failed_ = true;
    }
    throw;
  }
  if (::fsync(manifest_fd_) != 0) {
    failed_ = true;
    system_error("fsync MANIFEST");
  }
  manifest_bytes_ += record.size();
  levels_ = std::move(next);

  if (manifest_bytes_ >= kManifestRewriteBytes &&
      manifest_bytes_ > 4 * manifest_snapshot_bytes_) {
    try {
      rewrite_manifest();
    } catch (...) {
      // The edit above is already committed. A rewrite that failed before
      // its rename left the complete old log in place, so carry on.
      if (failed_) {
        throw;
      }
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
  wal_size_ = offset;
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
  require_writable();
  const std::uint64_t sequence = sequence_ + 1;
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
  try {
    write_all(wal_fd_, record.data(), record.size());
  } catch (...) {
    // A partial record must not stay in the log: later appends would land
    // after it, and recovery stops at the first damaged record, discarding
    // everything behind it, including acknowledged writes.
    if (::ftruncate(wal_fd_, static_cast<off_t>(wal_size_)) != 0) {
      failed_ = true;
    }
    throw;
  }
  if (options_.sync_writes && ::fsync(wal_fd_) != 0) {
    // After a failed fsync the kernel may have dropped the dirty pages and
    // cleared the error, so a later fsync could succeed without the data.
    failed_ = true;
    system_error("fsync WAL");
  }
  wal_size_ += record.size();
  sequence_ = sequence;

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
  // Shallower levels hold newer data, and within level 0 newer tables come
  // first, so the first table that contains the key has its newest version.
  for (std::size_t level = 0; level < levels_.size(); ++level) {
    const auto& tables = levels_[level];
    std::size_t first = 0;
    std::size_t last = tables.size();
    if (level > 0) {
      // Disjoint ranges: at most one table can hold the key.
      const auto found = std::lower_bound(
          tables.begin(), tables.end(), key,
          [](const auto& table, const std::string& target) {
            return table->largest < target;
          });
      if (found == tables.end() || key < (*found)->smallest) {
        continue;
      }
      first = static_cast<std::size_t>(found - tables.begin());
      last = first + 1;
    }
    for (std::size_t index = first; index < last; ++index) {
      const Table& table = *tables[index];
      ++bloom_checks_;
      if (!table.bloom.maybe_contains(key)) {
        ++bloom_negative_hits_;
        continue;
      }
      TableCursor cursor(table, block_cache_.get(), block_reads_);
      cursor.seek(key);
      if (cursor.valid() && cursor.key() == key) {
        return cursor.entry().tombstone
                   ? std::nullopt
                   : std::optional<std::string>(cursor.entry().value);
      }
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
  for (const auto& tables : levels_) {
    for (const auto& table : tables) {
      if (table->entry_count == 0 || table->largest < begin ||
          (!end.empty() && !(table->smallest < end))) {
        continue;
      }
      TableCursor cursor(*table, block_cache_.get(), block_reads_);
      for (cursor.seek(begin);
           cursor.valid() && (end.empty() || cursor.key() < end);
           cursor.next()) {
        const auto current = latest.find(cursor.key());
        if (current == latest.end() ||
            current->second.sequence < cursor.entry().sequence) {
          latest[cursor.key()] = cursor.entry();
        }
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

std::filesystem::path LSMTree::table_path(std::uint64_t generation,
                                          bool temporary) const {
  return directory_ / ("sst-" + std::to_string(generation) +
                       (temporary ? ".dat.tmp" : ".dat"));
}

void LSMTree::flush() {
  if (memtable_.empty()) {
    return;
  }
  require_writable();
  const std::uint64_t generation = next_generation_++;
  const auto temporary = table_path(generation, true);
  const auto destination = table_path(generation, false);
  build_table(temporary, options_, memtable_.size(), [&](TableWriter& writer) {
    for (const auto& [key, entry] : memtable_) {
      writer.add(key, entry);
    }
  });
  if (::rename(temporary.c_str(), destination.c_str()) != 0) {
    system_error("rename SSTable");
  }
  sync_directory();
  auto table = load_table(destination);
  table->smallest = memtable_.begin()->first;
  Levels next = levels_;
  next[0].insert(next[0].begin(), std::move(table));
  // The table is live once the MANIFEST lists it; only then may the WAL that
  // also holds its data be discarded.
  commit(std::move(next));
  reset_wal();
  memtable_.clear();
  memtable_size_ = 0;
  maybe_compact();
}

void LSMTree::compact() {
  std::vector<std::shared_ptr<Table>> inputs;
  std::size_t target = 1;
  for (std::size_t level = 0; level < levels_.size(); ++level) {
    for (const auto& table : levels_[level]) {
      inputs.push_back(table);
    }
    if (level > 0 && !levels_[level].empty()) {
      target = level;
    }
  }
  if (inputs.size() < 2) {
    return;
  }
  require_writable();
  // Everything is an input and nothing lies below the target, so every
  // tombstone can be dropped.
  run_compaction(inputs, target);
}

std::uint64_t LSMTree::level_limit(std::size_t level) const {
  std::uint64_t limit = options_.level1_bytes;
  for (std::size_t deeper = 1; deeper < level; ++deeper) {
    if (limit > std::numeric_limits<std::uint64_t>::max() /
                    options_.level_multiplier) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    limit *= options_.level_multiplier;
  }
  return limit;
}

void LSMTree::maybe_compact() {
  for (;;) {
    if (levels_[0].size() >= options_.compaction_trigger) {
      compact_level0();
      continue;
    }
    bool compacted = false;
    // The last level has no size limit.
    for (std::size_t level = 1; level + 1 < levels_.size(); ++level) {
      std::uint64_t bytes = 0;
      for (const auto& table : levels_[level]) {
        bytes += table->file_bytes;
      }
      if (bytes > level_limit(level)) {
        compact_level(level);
        compacted = true;
        break;
      }
    }
    if (!compacted) {
      return;
    }
  }
}

// Level 0 tables may overlap each other, so all of them move down together,
// merged with every level 1 table their combined range touches.
void LSMTree::compact_level0() {
  std::vector<std::shared_ptr<Table>> inputs = levels_[0];
  std::string smallest;
  std::string largest;
  bool any = false;
  for (const auto& table : levels_[0]) {
    if (table->entry_count == 0) {
      continue;
    }
    if (!any || table->smallest < smallest) {
      smallest = table->smallest;
    }
    if (!any || largest < table->largest) {
      largest = table->largest;
    }
    any = true;
  }
  if (any) {
    for (const auto& table : levels_[1]) {
      if (overlaps(*table, smallest, largest)) {
        inputs.push_back(table);
      }
    }
  }
  run_compaction(inputs, 1);
}

// Moves one table from `level` into level + 1, merging it with the tables
// there whose ranges overlap it.
void LSMTree::compact_level(std::size_t level) {
  const auto& tables = levels_[level];
  auto chosen = std::find_if(tables.begin(), tables.end(),
                             [&](const auto& table) {
                               return compact_pointer_[level] < table->smallest;
                             });
  if (chosen == tables.end()) {
    chosen = tables.begin();
  }
  const std::shared_ptr<Table> table = *chosen;
  compact_pointer_[level] = table->largest;

  std::vector<std::shared_ptr<Table>> inputs{table};
  for (const auto& below : levels_[level + 1]) {
    if (overlaps(*below, table->smallest, table->largest)) {
      inputs.push_back(below);
    }
  }
  if (inputs.size() > 1) {
    run_compaction(inputs, level + 1);
    return;
  }
  // Nothing overlaps below: move the file down by rewriting only the
  // MANIFEST.
  Levels next = levels_;
  auto& from = next[level];
  from.erase(std::find(from.begin(), from.end(), table));
  auto& to = next[level + 1];
  to.insert(std::upper_bound(to.begin(), to.end(), table,
                             [](const auto& left, const auto& right) {
                               return left->smallest < right->smallest;
                             }),
            table);
  commit(std::move(next));
  table->level = level + 1;
}

// True when no level below `level` can hold a version of `key`, so a
// tombstone written to `level` has nothing left to hide.
bool LSMTree::is_base_level(std::size_t level, const std::string& key) const {
  for (std::size_t deeper = level + 1; deeper < levels_.size(); ++deeper) {
    const auto& tables = levels_[deeper];
    const auto found = std::lower_bound(
        tables.begin(), tables.end(), key,
        [](const auto& table, const std::string& target) {
          return table->largest < target;
        });
    if (found != tables.end() && !(key < (*found)->smallest)) {
      return false;
    }
  }
  return true;
}

// Merges `inputs` into new tables at `output_level`, then swaps them in with
// one MANIFEST write. Until that write, the inputs remain the live tables;
// after it, the outputs are. The input files are deleted only afterwards.
void LSMTree::run_compaction(const std::vector<std::shared_ptr<Table>>& inputs,
                             std::size_t output_level) {
  std::uint64_t total_keys = 0;
  std::uint64_t input_bytes = 0;
  for (const auto& table : inputs) {
    total_keys += table->entry_count;
    input_bytes += table->file_bytes;
  }
  // Size each output's Bloom filter for its share of the keys, with slack.
  const double share =
      input_bytes == 0 ? 1.0
                       : std::min(1.0, static_cast<double>(options_.table_bytes) /
                                           static_cast<double>(input_bytes));
  const std::uint64_t keys_per_output = std::max<std::uint64_t>(
      1, std::min<std::uint64_t>(
             total_keys,
             static_cast<std::uint64_t>(static_cast<double>(total_keys) *
                                        share * 1.5) +
                 16));

  struct Output {
    std::uint64_t generation;
    std::filesystem::path temporary;
    std::string smallest;
    std::string largest;
  };
  std::vector<Output> outputs;
  std::unique_ptr<TableWriter> writer;
  const auto finish_output = [&] {
    writer->finish();
    compaction_bytes_written_ += writer->bytes_written();
    writer.reset();
  };
  const auto discard_outputs = [&] {
    writer.reset();
    for (const auto& output : outputs) {
      std::error_code ignored;
      std::filesystem::remove(output.temporary, ignored);
    }
  };

  try {
    // Streaming k-way merge, one block per input, bypassing the block cache.
    std::vector<TableCursor> cursors;
    cursors.reserve(inputs.size());
    for (const auto& table : inputs) {
      cursors.emplace_back(*table, nullptr, block_reads_);
      cursors.back().seek("");
    }
    for (;;) {
      const std::string* smallest = nullptr;
      for (const auto& cursor : cursors) {
        if (cursor.valid() &&
            (smallest == nullptr || cursor.key() < *smallest)) {
          smallest = &cursor.key();
        }
      }
      if (smallest == nullptr) {
        break;
      }
      const std::string key = *smallest;
      const Entry* newest = nullptr;
      for (const auto& cursor : cursors) {
        if (cursor.valid() && cursor.key() == key &&
            (newest == nullptr || cursor.entry().sequence > newest->sequence)) {
          newest = &cursor.entry();
        }
      }
      if (!(newest->tombstone && is_base_level(output_level, key))) {
        if (!writer) {
          const std::uint64_t generation = next_generation_++;
          outputs.push_back({generation, table_path(generation, true), key, key});
          writer = std::make_unique<TableWriter>(
              outputs.back().temporary, options_.block_bytes,
              options_.bloom_bits_per_key, keys_per_output);
        }
        writer->add(key, *newest);
        outputs.back().largest = key;
        if (writer->bytes_written() >= options_.table_bytes) {
          finish_output();
        }
      }
      for (auto& cursor : cursors) {
        if (cursor.valid() && cursor.key() == key) {
          cursor.next();
        }
      }
    }
    if (writer) {
      finish_output();
    }
  } catch (...) {
    discard_outputs();
    throw;
  }

  // Rename every output into place. Any that are renamed but never reach the
  // MANIFEST are removed as orphans on the next open.
  std::vector<std::shared_ptr<Table>> installed;
  for (std::size_t index = 0; index < outputs.size(); ++index) {
    const auto destination = table_path(outputs[index].generation, false);
    if (::rename(outputs[index].temporary.c_str(), destination.c_str()) != 0) {
      const int saved = errno;
      for (std::size_t rest = index; rest < outputs.size(); ++rest) {
        std::error_code ignored;
        std::filesystem::remove(outputs[rest].temporary, ignored);
      }
      errno = saved;
      system_error("rename SSTable");
    }
  }
  if (!outputs.empty()) {
    sync_directory();
  }
  for (const auto& output : outputs) {
    auto table = load_table(table_path(output.generation, false));
    table->level = output_level;
    table->smallest = output.smallest;
    installed.push_back(std::move(table));
  }

  Levels next = levels_;
  for (auto& tables : next) {
    tables.erase(std::remove_if(tables.begin(), tables.end(),
                                [&](const auto& table) {
                                  return std::find(inputs.begin(), inputs.end(),
                                                   table) != inputs.end();
                                }),
                 tables.end());
  }
  auto& level = next[output_level];
  level.insert(level.end(), installed.begin(), installed.end());
  std::sort(level.begin(), level.end(), [](const auto& left, const auto& right) {
    return left->smallest < right->smallest;
  });
  commit(std::move(next));

  // The inputs are no longer live. A file that fails to delete is an orphan
  // and is removed on the next open.
  for (const auto& table : inputs) {
    std::error_code ignored;
    std::filesystem::remove(table->path, ignored);
  }
  sync_directory();
}

void LSMTree::reset_wal() {
  if (::ftruncate(wal_fd_, 0) != 0 || ::fsync(wal_fd_) != 0) {
    failed_ = true;
    system_error("reset WAL");
  }
  wal_size_ = 0;
}

void LSMTree::require_writable() const {
  if (failed_) {
    throw std::runtime_error(
        "database refused the write after an earlier I/O error; reopen it "
        "to recover");
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
  Stats stats;
  stats.memtable_entries = memtable_.size();
  stats.sequence = sequence_;
  stats.bloom_checks = bloom_checks_;
  stats.bloom_negative_hits = bloom_negative_hits_;
  stats.block_reads = block_reads_;
  stats.block_cache_hits = block_cache_->hits;
  stats.compaction_bytes_written = compaction_bytes_written_;
  for (const auto& tables : levels_) {
    stats.tables_per_level.push_back(tables.size());
    stats.sstable_count += tables.size();
  }
  return stats;
}

}  // namespace lsm
