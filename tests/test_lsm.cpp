#include "lsm/lsm.h"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

namespace {

int assertions = 0;
int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    ++assertions;                                                            \
    if (!(condition)) {                                                      \
      ++failures;                                                            \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: "       \
                << #condition << "\n";                                      \
    }                                                                        \
  } while (false)

std::filesystem::path test_directory(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("lsm-tree-" + name + "-" +
          std::to_string(static_cast<long long>(::getpid())));
}

void reset(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

lsm::Options small_options() {
  lsm::Options options;
  options.memtable_bytes = 90;
  options.compaction_trigger = 4;
  // Tiny blocks so most tables span several blocks and cursors cross
  // block boundaries during reads, scans and compaction.
  options.block_bytes = 64;
  return options;
}

void test_put_get_delete() {
  const auto directory = test_directory("basic");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("alpha", "one");
    database.put("beta", "two");
    CHECK(database.get("alpha") == std::optional<std::string>("one"));
    CHECK(database.get("beta") == std::optional<std::string>("two"));
    CHECK(!database.get("missing"));
    database.erase("alpha");
    CHECK(!database.get("alpha"));
  }
  reset(directory);
}

void test_wal_recovers_without_flush() {
  const auto directory = test_directory("wal");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("survives", "restart");
  }
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("survives") ==
          std::optional<std::string>("restart"));
    CHECK(database.stats().memtable_entries == 1);
  }
  reset(directory);
}

void test_flush_and_reopen() {
  const auto directory = test_directory("flush");
  reset(directory);
  {
    lsm::LSMTree database(directory, small_options());
    for (int index = 0; index < 40; ++index) {
      database.put("key-" + std::to_string(index),
                   "value-" + std::to_string(index));
    }
    database.flush();
    CHECK(database.stats().sstable_count >= 1);
  }
  {
    lsm::LSMTree database(directory, small_options());
    for (int index = 0; index < 40; ++index) {
      CHECK(database.get("key-" + std::to_string(index)) ==
            std::optional<std::string>("value-" + std::to_string(index)));
    }
  }
  reset(directory);
}

void test_newest_value_and_tombstone_win() {
  const auto directory = test_directory("versions");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 1024;
  options.compaction_trigger = 10;
  {
    lsm::LSMTree database(directory, options);
    database.put("shared", "old");
    database.flush();
    database.put("shared", "new");
    database.flush();
    CHECK(database.get("shared") == std::optional<std::string>("new"));
    database.erase("shared");
    database.flush();
    CHECK(!database.get("shared"));
  }
  {
    lsm::LSMTree database(directory, options);
    CHECK(!database.get("shared"));
    database.compact();
    CHECK(!database.get("shared"));
    CHECK(database.stats().sstable_count == 1);
  }
  reset(directory);
}

void test_torn_wal_tail_is_repaired() {
  const auto directory = test_directory("torn-wal");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("complete", "record");
  }
  const auto wal = directory / "active.wal";
  {
    std::ofstream output(wal, std::ios::binary | std::ios::app);
    output.write("WAL1broken-tail", 15);
  }
  const auto damaged_size = std::filesystem::file_size(wal);
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("complete") ==
          std::optional<std::string>("record"));
  }
  CHECK(std::filesystem::file_size(wal) < damaged_size);
  reset(directory);
}

void test_bloom_filter_has_no_false_negatives() {
  const auto directory = test_directory("bloom");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 4096;
  options.compaction_trigger = 10;
  lsm::LSMTree database(directory, options);
  for (int index = 0; index < 100; ++index) {
    database.put("present-" + std::to_string(index), "value");
  }
  database.flush();
  for (int index = 0; index < 100; ++index) {
    CHECK(database.get("present-" + std::to_string(index)).has_value());
  }
  for (int index = 0; index < 100; ++index) {
    CHECK(!database.get("absent-" + std::to_string(index)).has_value());
  }
  const auto stats = database.stats();
  CHECK(stats.bloom_checks >= 200);
  CHECK(stats.bloom_negative_hits > 0);
  reset(directory);
}

void test_range_scan_reconciles_versions_and_tombstones() {
  const auto directory = test_directory("scan");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 4096;
  options.compaction_trigger = 10;
  {
    lsm::LSMTree database(directory, options);
    database.put("a", "old-a");
    database.put("b", "old-b");
    database.put("c", "old-c");
    database.put("d", "old-d");
    database.flush();
    database.put("b", "new-b");
    database.erase("c");
    database.put("e", "new-e");
    const auto result = database.scan("b", "e");
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"b", "new-b"}, {"d", "old-d"}};
    CHECK(result == expected);
    database.flush();
  }
  {
    lsm::LSMTree database(directory, options);
    const auto result = database.scan("", "");
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"a", "old-a"}, {"b", "new-b"}, {"d", "old-d"}, {"e", "new-e"}};
    CHECK(result == expected);
    database.compact();
    CHECK(database.scan("", "") == expected);
  }
  reset(directory);
}

void test_randomized_differential_replay() {
  const auto directory = test_directory("differential");
  reset(directory);
  auto options = small_options();
  options.sync_writes = false;
  std::map<std::string, std::optional<std::string>> oracle;
  std::mt19937 random(20260728);

  for (int phase = 0; phase < 5; ++phase) {
    {
      lsm::LSMTree database(directory, options);
      for (int step = 0; step < 400; ++step) {
        const std::string key = "key-" + std::to_string(random() % 80);
        if (random() % 5 == 0) {
          database.erase(key);
          oracle[key] = std::nullopt;
        } else {
          const std::string value = "value-" + std::to_string(random());
          database.put(key, value);
          oracle[key] = value;
        }
        if (step % 47 == 0) {
          for (const auto& [expected_key, expected_value] : oracle) {
            CHECK(database.get(expected_key) == expected_value);
          }
        }
      }
    }
    lsm::LSMTree reopened(directory, options);
    for (const auto& [key, value] : oracle) {
      CHECK(reopened.get(key) == value);
    }
  }
  reset(directory);
}

void test_corrupt_sstable_is_rejected() {
  const auto directory = test_directory("corrupt-table");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("key", "value");
    database.flush();
  }
  std::filesystem::path table_path;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (item.path().extension() == ".dat") {
      table_path = item.path();
    }
  }
  CHECK(!table_path.empty());
  {
    std::fstream file(table_path,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(-1, std::ios::end);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 0x5A;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
  }
  bool rejected = false;
  try {
    lsm::LSMTree database(directory);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  CHECK(rejected);
  reset(directory);
}

void test_failed_wal_write_does_not_lose_later_writes() {
  const auto directory = test_directory("failed-write");
  reset(directory);
  const auto wal = directory / "active.wal";
  {
    lsm::LSMTree database(directory);
    database.put("before", "kept");
    const auto clean_size = std::filesystem::file_size(wal);

    // Cap the file size so the next WAL append is cut off part-way.
    struct rlimit original {};
    CHECK(::getrlimit(RLIMIT_FSIZE, &original) == 0);
    struct rlimit limited = original;
    limited.rlim_cur = static_cast<rlim_t>(clean_size + 10);
    const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
    CHECK(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    bool failed = false;
    try {
      database.put("torn", std::string(64, 'x'));
    } catch (const std::runtime_error&) {
      failed = true;
    }
    CHECK(::setrlimit(RLIMIT_FSIZE, &original) == 0);
    std::signal(SIGXFSZ, previous_handler);

    CHECK(failed);
    CHECK(std::filesystem::file_size(wal) == clean_size);
    CHECK(!database.get("torn"));
    database.put("after", "acknowledged");
  }
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("before") == std::optional<std::string>("kept"));
    CHECK(database.get("after") ==
          std::optional<std::string>("acknowledged"));
    CHECK(!database.get("torn"));
  }
  reset(directory);
}

void test_directory_lock_rejects_second_open() {
  const auto directory = test_directory("lock");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("owner", "first");
    bool rejected = false;
    try {
      lsm::LSMTree second(directory);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(database.get("owner") == std::optional<std::string>("first"));
  }
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("owner") == std::optional<std::string>("first"));
  }
  reset(directory);
}

void test_abandoned_temporary_table_is_removed() {
  const auto directory = test_directory("tmp-table");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("durable", "value");
    database.flush();
  }
  const auto abandoned = directory / "sst-99.dat.tmp";
  {
    std::ofstream output(abandoned, std::ios::binary);
    output << "half-written table";
  }
  {
    lsm::LSMTree database(directory);
    CHECK(!std::filesystem::exists(abandoned));
    CHECK(database.get("durable") == std::optional<std::string>("value"));
    CHECK(database.stats().sstable_count == 1);
  }
  reset(directory);
}

void put_u32(std::string& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

void put_u64(std::string& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

std::uint32_t table_version(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  char header[8] = {};
  input.read(header, sizeof(header));
  std::uint32_t version = 0;
  for (int index = 3; index >= 0; --index) {
    version = (version << 8) | static_cast<unsigned char>(header[4 + index]);
  }
  return version;
}

std::vector<std::filesystem::path> table_files(
    const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> files;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (item.path().extension() == ".dat") {
      files.push_back(item.path());
    }
  }
  return files;
}

// Hand-builds a table in the original (version 1) format, as written by
// earlier releases: [header | Bloom | entries | whole-file CRC32].
void write_v1_table(const std::filesystem::path& path) {
  struct Row {
    std::uint64_t sequence;
    bool tombstone;
    std::string key;
    std::string value;
  };
  const std::vector<Row> rows = {{1, false, "apple", "red"},
                                 {2, true, "banana", ""},
                                 {3, false, "cherry", "dark"}};
  std::string bytes;
  put_u32(bytes, 0x31545353U);  // SST1
  put_u32(bytes, 1);            // version 1
  put_u64(bytes, rows.size());
  put_u32(bytes, 64);  // Bloom bits
  put_u32(bytes, 1);   // Bloom hashes
  bytes.append(8, static_cast<char>(0xFF));  // All bits set: "maybe" for all.
  for (const auto& row : rows) {
    put_u64(bytes, row.sequence);
    bytes.push_back(row.tombstone ? 1 : 0);
    bytes.append(3, '\0');
    put_u32(bytes, static_cast<std::uint32_t>(row.key.size()));
    put_u32(bytes, static_cast<std::uint32_t>(row.value.size()));
    bytes += row.key;
    bytes += row.value;
  }
  put_u32(bytes, lsm::crc32(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                            bytes.size()));
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void test_version1_table_is_read_and_migrated() {
  const auto directory = test_directory("v1-table");
  reset(directory);
  std::filesystem::create_directories(directory);
  write_v1_table(directory / "sst-1.dat");
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("apple") == std::optional<std::string>("red"));
    CHECK(!database.get("banana"));
    CHECK(database.get("cherry") == std::optional<std::string>("dark"));
    CHECK(database.stats().sequence == 3);
    database.put("date", "brown");
    CHECK(database.stats().sequence == 4);
    database.flush();
    database.compact();
    const auto files = table_files(directory);
    CHECK(files.size() == 1);
    CHECK(!files.empty() && table_version(files.front()) == 2);
  }
  {
    lsm::LSMTree database(directory);
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"apple", "red"}, {"cherry", "dark"}, {"date", "brown"}};
    CHECK(database.scan("", "") == expected);
    CHECK(!database.get("banana"));
  }
  reset(directory);
}

lsm::Options block_options() {
  lsm::Options options;
  options.memtable_bytes = 1 << 20;
  options.compaction_trigger = 10;
  options.block_bytes = 256;
  return options;
}

std::string numbered(const std::string& prefix, int number) {
  std::string digits = std::to_string(number);
  return prefix + std::string(4 - digits.size(), '0') + digits;
}

void test_point_read_loads_one_block() {
  const auto directory = test_directory("block-reads");
  reset(directory);
  {
    lsm::LSMTree database(directory, block_options());
    for (int index = 0; index < 500; ++index) {
      database.put(numbered("key-", index), std::string(40, 'v'));
    }
    database.flush();
  }
  lsm::LSMTree database(directory, block_options());
  CHECK(database.stats().block_reads == 0);  // Opening reads no data blocks.

  CHECK(database.get("key-0250") == std::optional<std::string>(std::string(40, 'v')));
  CHECK(database.stats().block_reads == 1);
  // A second read of the same block is served by the block cache.
  CHECK(database.get("key-0250").has_value());
  CHECK(database.stats().block_reads == 1);
  CHECK(database.stats().block_cache_hits == 1);

  for (int index = 0; index < 200; ++index) {
    CHECK(!database.get(numbered("missing-", index)));
  }
  const auto after_misses = database.stats();
  CHECK(after_misses.bloom_negative_hits >= 180);
  CHECK(after_misses.block_reads <= 1 + 20);

  const auto page = database.scan("key-0100", "key-0110");
  CHECK(page.size() == 10);
  CHECK(database.stats().block_reads - after_misses.block_reads <= 4);

  const auto before_full = database.stats().block_reads;
  CHECK(database.scan("", "").size() == 500);
  // 500 entries of ~70 bytes in 256-byte blocks: well over 50 blocks.
  CHECK(database.stats().block_reads - before_full >= 50);
  reset(directory);
}

void test_disabled_block_cache_reads_from_disk() {
  const auto directory = test_directory("no-cache");
  reset(directory);
  auto options = block_options();
  options.block_cache_bytes = 0;
  {
    lsm::LSMTree database(directory, options);
    database.put("key", "value");
    database.flush();
  }
  lsm::LSMTree database(directory, options);
  CHECK(database.get("key") == std::optional<std::string>("value"));
  CHECK(database.get("key") == std::optional<std::string>("value"));
  CHECK(database.stats().block_reads == 2);
  CHECK(database.stats().block_cache_hits == 0);
  reset(directory);
}

void test_corrupt_data_block_is_detected_on_read() {
  const auto directory = test_directory("corrupt-block");
  reset(directory);
  {
    lsm::LSMTree database(directory, block_options());
    for (int index = 0; index < 100; ++index) {
      database.put(numbered("key-", index), "value-" + std::to_string(index));
    }
    database.flush();
  }
  const auto files = table_files(directory);
  CHECK(files.size() == 1);
  {
    // Byte 30 is inside the first entry of the first data block.
    std::fstream file(files.front(),
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(30);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 0x5A;
    file.seekp(30);
    file.write(&byte, 1);
  }
  // The footer, index and Bloom filter are intact, so the table opens.
  lsm::LSMTree database(directory, block_options());
  bool rejected = false;
  try {
    database.get("key-0000");
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  CHECK(rejected);
  // Keys in other blocks remain readable.
  CHECK(database.get("key-0099") == std::optional<std::string>("value-99"));
  bool scan_rejected = false;
  try {
    database.scan("", "");
  } catch (const std::runtime_error&) {
    scan_rejected = true;
  }
  CHECK(scan_rejected);
  reset(directory);
}

void test_value_larger_than_a_block() {
  const auto directory = test_directory("large-value");
  reset(directory);
  std::string large(100000, 'x');
  large[0] = 'a';
  large[large.size() - 1] = 'z';
  {
    lsm::LSMTree database(directory, block_options());
    database.put("big", large);
    database.put("small", "s");
    database.flush();
    database.put("after", "a");
    database.flush();
    database.compact();
  }
  lsm::LSMTree database(directory, block_options());
  CHECK(database.get("big") == std::optional<std::string>(large));
  CHECK(database.get("small") == std::optional<std::string>("s"));
  CHECK(database.get("after") == std::optional<std::string>("a"));
  reset(directory);
}

}  // namespace

int main() {
  test_put_get_delete();
  test_wal_recovers_without_flush();
  test_flush_and_reopen();
  test_newest_value_and_tombstone_win();
  test_torn_wal_tail_is_repaired();
  test_bloom_filter_has_no_false_negatives();
  test_range_scan_reconciles_versions_and_tombstones();
  test_randomized_differential_replay();
  test_corrupt_sstable_is_rejected();
  test_failed_wal_write_does_not_lose_later_writes();
  test_directory_lock_rejects_second_open();
  test_abandoned_temporary_table_is_removed();
  test_version1_table_is_read_and_migrated();
  test_point_read_loads_one_block();
  test_disabled_block_cache_reads_from_disk();
  test_corrupt_data_block_is_detected_on_read();
  test_value_larger_than_a_block();
  if (failures != 0) {
    std::cerr << failures << " of " << assertions << " assertions failed\n";
    return 1;
  }
  std::cout << assertions << " assertions passed\n";
  return 0;
}
