#include "lsm/lsm.h"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

// The WAL file new writes go to: the highest-numbered wal-N.log.
std::filesystem::path newest_wal(const std::filesystem::path& directory) {
  std::filesystem::path newest;
  unsigned long long highest = 0;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    const std::string name = item.path().filename().string();
    if (name.rfind("wal-", 0) == 0 && item.path().extension() == ".log") {
      const unsigned long long number = std::stoull(name.substr(4));
      if (newest.empty() || number > highest) {
        highest = number;
        newest = item.path();
      }
    }
  }
  return newest;
}

lsm::Options small_options() {
  lsm::Options options;
  options.memtable_bytes = 90;
  options.compaction_trigger = 4;
  // Tiny blocks so most tables span several blocks and cursors cross
  // block boundaries during reads, scans and compaction.
  options.block_bytes = 64;
  // Tiny tables and levels so data moves through many levels, exercising
  // multi-table compactions, trivial moves and tombstone collection.
  options.table_bytes = 256;
  options.level1_bytes = 512;
  options.level_multiplier = 2;
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
    // A full compaction leaves nothing below that the tombstone must hide,
    // so the tombstone and every older version are dropped entirely.
    CHECK(database.stats().sstable_count == 0);
  }
  {
    lsm::LSMTree database(directory, options);
    CHECK(!database.get("shared"));
    CHECK(database.scan("", "").empty());
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
  const auto wal = newest_wal(directory);
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
  {
    lsm::LSMTree database(directory);
    const auto wal = newest_wal(directory);
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
    CHECK(!files.empty() && table_version(files.front()) == 3);
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

std::size_t deepest_level(const lsm::Stats& stats) {
  std::size_t deepest = 0;
  for (std::size_t level = 0; level < stats.tables_per_level.size(); ++level) {
    if (stats.tables_per_level[level] != 0) {
      deepest = level;
    }
  }
  return deepest;
}

void test_data_moves_through_levels() {
  const auto directory = test_directory("levels");
  reset(directory);
  auto options = small_options();
  options.sync_writes = false;
  std::map<std::string, std::optional<std::string>> oracle;
  std::mt19937 random(424242);
  std::vector<std::size_t> levels_before;
  {
    lsm::LSMTree database(directory, options);
    for (int step = 0; step < 3000; ++step) {
      const std::string key = numbered("level-", static_cast<int>(random() % 600));
      if (random() % 6 == 0) {
        database.erase(key);
        oracle[key] = std::nullopt;
      } else {
        const std::string value = "v" + std::to_string(random() % 100000);
        database.put(key, value);
        oracle[key] = value;
      }
    }
    database.wait_for_compactions();
    const auto stats = database.stats();
    CHECK(deepest_level(stats) >= 3);
    CHECK(stats.tables_per_level[0] < options.compaction_trigger);
    CHECK(stats.compaction_bytes_written > 0);
    for (const auto& [key, value] : oracle) {
      CHECK(database.get(key) == value);
    }
    levels_before = stats.tables_per_level;
  }
  // Reopening rebuilds the same levels from the MANIFEST (and checks that
  // no two tables in a level >= 1 overlap).
  lsm::LSMTree database(directory, options);
  CHECK(database.stats().tables_per_level == levels_before);
  std::vector<std::pair<std::string, std::string>> expected;
  for (const auto& [key, value] : oracle) {
    CHECK(database.get(key) == value);
    if (value) {
      expected.emplace_back(key, *value);
    }
  }
  CHECK(database.scan("", "") == expected);
  reset(directory);
}

void test_manifest_decides_which_tables_are_live() {
  // A table that was renamed into place but never reached the MANIFEST (a
  // compaction that crashed before committing) must be ignored and removed,
  // even though its generation is the newest in the directory.
  const auto stray_source = test_directory("stray-source");
  const auto directory = test_directory("stray");
  reset(stray_source);
  reset(directory);
  {
    lsm::LSMTree database(stray_source);
    database.put("key", "stale");
    database.flush();
  }
  {
    lsm::LSMTree database(directory);
    database.put("key", "live");
    database.flush();
  }
  const auto stray = directory / "sst-50.dat";
  std::filesystem::copy_file(stray_source / "sst-1.dat", stray);
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("key") == std::optional<std::string>("live"));
    CHECK(!std::filesystem::exists(stray));
    CHECK(database.stats().sstable_count == 1);
  }
  reset(stray_source);
  reset(directory);
}

void test_directory_without_manifest_is_migrated() {
  const auto directory = test_directory("no-manifest");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("a", "1");
    database.flush();
    database.put("a", "2");
    database.put("b", "3");
    database.flush();
  }
  // Directories written before the MANIFEST existed have none.
  std::filesystem::remove(directory / "MANIFEST");
  {
    lsm::LSMTree database(directory);
    CHECK(std::filesystem::exists(directory / "MANIFEST"));
    CHECK(database.get("a") == std::optional<std::string>("2"));
    CHECK(database.get("b") == std::optional<std::string>("3"));
    CHECK(database.stats().tables_per_level[0] == 2);
  }
  reset(directory);
}

void test_damaged_manifest_is_rejected() {
  const auto directory = test_directory("bad-manifest");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("key", "value");
    database.flush();
  }
  {
    std::fstream file(directory / "MANIFEST",
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(12);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 0x5A;
    file.seekp(12);
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

  // A table the MANIFEST lists but that is missing is also fatal: guessing
  // could resurrect older data.
  {
    lsm::LSMTree database(directory);
    database.put("key", "value");
    database.flush();
  }
  std::filesystem::remove(directory / "sst-1.dat");
  bool missing_rejected = false;
  try {
    lsm::LSMTree database(directory);
  } catch (const std::runtime_error&) {
    missing_rejected = true;
  }
  CHECK(missing_rejected);
  reset(directory);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t get_u32(const std::string& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 3; index >= 0; --index) {
    value = (value << 8) | static_cast<unsigned char>(bytes[offset + index]);
  }
  return value;
}

// Offsets of each record in a version 2 MANIFEST.
std::vector<std::size_t> manifest_records(const std::string& bytes) {
  std::vector<std::size_t> offsets;
  std::size_t position = 8;
  while (position + 9 <= bytes.size()) {
    offsets.push_back(position);
    position += 5 + get_u32(bytes, position) + 4;
  }
  return offsets;
}

void test_manifest_appends_edits() {
  const auto directory = test_directory("manifest-log");
  reset(directory);
  lsm::Options options;
  options.compaction_trigger = 10;
  const auto manifest = directory / "MANIFEST";
  {
    lsm::LSMTree database(directory, options);
    std::uintmax_t previous = std::filesystem::file_size(manifest);
    for (int round = 0; round < 5; ++round) {
      database.put(numbered("key-", round), "value");
      database.flush();
      const auto size = std::filesystem::file_size(manifest);
      // One small edit is appended per flush; nothing is rewritten.
      CHECK(size > previous);
      CHECK(size - previous < 100);
      previous = size;
    }
  }
  const std::string bytes = read_file(manifest);
  CHECK(get_u32(bytes, 0) == 0x324E414DU);  // MAN2
  CHECK(manifest_records(bytes).size() == 6);  // snapshot + five edits
  lsm::LSMTree database(directory, options);
  CHECK(database.stats().sstable_count == 5);
  CHECK(database.get("key-0004") == std::optional<std::string>("value"));
  reset(directory);
}

void test_manifest_log_stays_bounded() {
  const auto directory = test_directory("manifest-bounded");
  reset(directory);
  auto options = small_options();
  options.sync_writes = false;
  std::uintmax_t largest = 0;
  {
    lsm::LSMTree database(directory, options);
    for (int index = 0; index < 6000; ++index) {
      database.put(numbered("bounded-", index % 900), std::to_string(index));
      largest = std::max(largest, std::filesystem::file_size(directory / "MANIFEST"));
    }
  }
  // Thousands of edits, but the log is rewritten as a snapshot once it is
  // well past 64 KiB.
  CHECK(largest < 256 * 1024);
  lsm::LSMTree database(directory, options);
  for (int index = 5100; index < 6000; ++index) {
    CHECK(database.get(numbered("bounded-", index % 900)) ==
          std::optional<std::string>(std::to_string(index)));
  }
  reset(directory);
}

void test_torn_manifest_tail_is_ignored() {
  const auto directory = test_directory("manifest-torn");
  reset(directory);
  lsm::Options options;
  options.compaction_trigger = 10;
  {
    lsm::LSMTree database(directory, options);
    database.put("a", "1");
    database.flush();
    database.put("b", "2");
    database.flush();
  }
  const auto manifest = directory / "MANIFEST";
  const std::string clean = read_file(manifest);
  for (int variant = 0; variant < 2; ++variant) {
    std::string damaged = clean;
    if (variant == 0) {
      // An append cut short: the header promises more bytes than exist.
      put_u32(damaged, 200);
      damaged += "\x02partial";
    } else {
      // A complete final record whose checksum does not match.
      std::string payload(30, 'x');
      put_u32(damaged, static_cast<std::uint32_t>(payload.size()));
      damaged += '\x02';
      damaged += payload;
      put_u32(damaged, 0xDEADBEEFU);
    }
    write_file(manifest, damaged);
    {
      lsm::LSMTree database(directory, options);
      CHECK(database.get("a") == std::optional<std::string>("1"));
      CHECK(database.get("b") == std::optional<std::string>("2"));
      CHECK(database.stats().sstable_count == 2);
    }
    // The uncommitted edit is gone; the log was rewritten cleanly.
    CHECK(read_file(manifest).size() < damaged.size());
  }
  reset(directory);
}

void test_corrupt_manifest_record_before_tail_fails_open() {
  const auto directory = test_directory("manifest-middle");
  reset(directory);
  lsm::Options options;
  options.compaction_trigger = 10;
  {
    lsm::LSMTree database(directory, options);
    for (int round = 0; round < 3; ++round) {
      database.put(numbered("key-", round), "value");
      database.flush();
    }
  }
  const auto manifest = directory / "MANIFEST";
  std::string bytes = read_file(manifest);
  const auto records = manifest_records(bytes);
  CHECK(records.size() == 4);
  // Damage the first edit; two valid edits follow it. Treating this as a
  // torn tail would drop the later tables and delete them as orphans.
  bytes[records[1] + 9] ^= 0x5A;
  write_file(manifest, bytes);
  bool rejected = false;
  try {
    lsm::LSMTree database(directory, options);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  CHECK(rejected);
  CHECK(table_files(directory).size() == 3);  // Nothing was deleted.
  reset(directory);
}

void test_version1_manifest_is_upgraded() {
  const auto directory = test_directory("manifest-v1");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("k", "v");
    database.flush();
  }
  // The single-snapshot format written by the previous release.
  std::string bytes;
  put_u32(bytes, 0x314E414DU);  // MAN1
  put_u32(bytes, 1);
  put_u64(bytes, 2);  // next generation
  put_u32(bytes, 1);  // one table
  put_u64(bytes, 1);  // generation
  put_u32(bytes, 0);  // level
  put_u32(bytes, 1);
  bytes += "k";
  put_u32(bytes, 1);
  bytes += "k";
  put_u32(bytes, lsm::crc32(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                            bytes.size()));
  write_file(directory / "MANIFEST", bytes);
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("k") == std::optional<std::string>("v"));
  }
  CHECK(get_u32(read_file(directory / "MANIFEST"), 0) == 0x324E414DU);
  reset(directory);
}

// Hand-builds a one-block table in the version 2 format written by earlier
// releases: [header | block (fixed entries + CRC) | index + CRC | Bloom |
// 64-byte footer].
void write_v2_table(const std::filesystem::path& path) {
  struct Row {
    std::uint64_t sequence;
    bool tombstone;
    std::string key;
    std::string value;
  };
  const std::vector<Row> rows = {{1, false, "alpha", "one"},
                                 {2, true, "beta", ""},
                                 {3, false, "gamma", "three"}};
  const auto crc = [](const std::string& bytes) {
    return lsm::crc32(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                      bytes.size());
  };
  std::string block;
  for (const auto& row : rows) {
    put_u64(block, row.sequence);
    block.push_back(row.tombstone ? 1 : 0);
    block.append(3, '\0');
    put_u32(block, static_cast<std::uint32_t>(row.key.size()));
    put_u32(block, static_cast<std::uint32_t>(row.value.size()));
    block += row.key;
    block += row.value;
  }
  put_u32(block, crc(block));
  std::string index;
  put_u32(index, 5);
  index += "gamma";
  put_u64(index, 8);
  put_u32(index, static_cast<std::uint32_t>(block.size()));
  put_u32(index, crc(index));
  const std::string bloom(8, static_cast<char>(0xFF));  // "maybe" for all
  std::string bytes;
  put_u32(bytes, 0x31545353U);  // SST1
  put_u32(bytes, 2);            // version 2
  bytes += block;
  const std::uint64_t index_offset = bytes.size();
  bytes += index;
  const std::uint64_t bloom_offset = bytes.size();
  bytes += bloom;
  std::string footer;
  put_u64(footer, index_offset);
  put_u64(footer, index.size());
  put_u64(footer, bloom_offset);
  put_u64(footer, bloom.size());
  put_u64(footer, rows.size());
  put_u64(footer, 3);   // max sequence
  put_u32(footer, 64);  // Bloom bits
  put_u32(footer, 1);   // Bloom hashes
  put_u32(footer, 0x46545353U);
  put_u32(footer, crc(footer));
  bytes += footer;
  write_file(path, bytes);
}

void test_version2_table_is_read_and_migrated() {
  const auto directory = test_directory("v2-table");
  reset(directory);
  std::filesystem::create_directories(directory);
  write_v2_table(directory / "sst-1.dat");
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("alpha") == std::optional<std::string>("one"));
    CHECK(!database.get("beta"));
    CHECK(database.get("gamma") == std::optional<std::string>("three"));
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"alpha", "one"}, {"gamma", "three"}};
    CHECK(database.scan("", "") == expected);
    CHECK(database.stats().sequence == 3);
    database.put("delta", "four");
    database.flush();
    database.compact();
    const auto files = table_files(directory);
    CHECK(files.size() == 1);
    CHECK(!files.empty() && table_version(files.front()) == 3);
  }
  lsm::LSMTree database(directory);
  CHECK(database.get("alpha") == std::optional<std::string>("one"));
  CHECK(database.get("delta") == std::optional<std::string>("four"));
  CHECK(!database.get("beta"));
  reset(directory);
}

void test_prefix_compression_shrinks_tables() {
  const auto directory = test_directory("prefix");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 1 << 20;
  std::size_t raw_bytes = 0;
  {
    lsm::LSMTree database(directory, options);
    for (int index = 0; index < 2000; ++index) {
      const std::string key = "customer:profile:" + numbered("", index);
      const std::string value = "v" + std::to_string(index);
      database.put(key, value);
      raw_bytes += key.size() + value.size();
    }
    database.flush();
  }
  const auto files = table_files(directory);
  CHECK(files.size() == 1);
  // The previous format spent 20 bytes per entry on headers and stored every
  // key whole, so its tables were larger than the raw data. Shared prefixes
  // and varint lengths bring this one well below it.
  CHECK(!files.empty() && std::filesystem::file_size(files.front()) <
                              raw_bytes * 7 / 10);
  reset(directory);
}

void test_restart_points_find_every_key() {
  const auto directory = test_directory("restarts");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 1 << 20;
  options.block_bytes = 1 << 20;  // One block with many restart points.
  options.block_cache_bytes = 0;  // Every lookup searches the raw block.
  {
    lsm::LSMTree database(directory, options);
    for (int index = 0; index < 1000; index += 2) {
      database.put(numbered("key-", index), std::to_string(index));
    }
    database.flush();
  }
  lsm::LSMTree database(directory, options);
  for (int index = 0; index < 1000; ++index) {
    const auto value = database.get(numbered("key-", index));
    if (index % 2 == 0) {
      CHECK(value == std::optional<std::string>(std::to_string(index)));
    } else {
      CHECK(!value);  // Falls between two stored keys.
    }
  }
  CHECK(!database.get("key-"));       // Before the first key.
  CHECK(!database.get("key-9999"));   // After the last key.
  // Ranges that start and end in the middle of restart segments.
  for (int begin = 1; begin < 1000; begin += 37) {
    const auto page = database.scan(numbered("key-", begin), numbered("key-", begin + 41));
    std::size_t expected = 0;
    for (int index = begin; index < begin + 41 && index < 1000; ++index) {
      expected += index % 2 == 0 ? 1 : 0;
    }
    CHECK(page.size() == expected);
  }
  reset(directory);
}

void test_snapshot_sees_a_frozen_view() {
  const auto directory = test_directory("snapshot");
  reset(directory);
  lsm::Options options;
  options.compaction_trigger = 10;
  lsm::LSMTree database(directory, options);
  database.put("a", "1");
  database.put("b", "1");
  const auto snapshot = database.snapshot();
  database.put("a", "2");
  database.erase("b");
  database.put("c", "3");
  const std::vector<std::pair<std::string, std::string>> then = {{"a", "1"},
                                                                 {"b", "1"}};
  std::vector<std::pair<std::string, std::string>> now = {{"a", "2"},
                                                          {"c", "3"}};
  // In the memtable, after a flush, and after full compactions, the
  // snapshot sees the old view and plain reads see the new one.
  for (int stage = 0; stage < 3; ++stage) {
    CHECK(database.get("a", *snapshot) == std::optional<std::string>("1"));
    CHECK(database.get("b", *snapshot) == std::optional<std::string>("1"));
    CHECK(!database.get("c", *snapshot));
    CHECK(database.scan("", "", *snapshot) == then);
    CHECK(database.get("a") == std::optional<std::string>("2"));
    CHECK(!database.get("b"));
    CHECK(database.scan("", "") == now);
    if (stage == 0) {
      database.flush();
    } else {
      database.put("d", std::to_string(stage));
      if (now.size() == 2) {
        now.emplace_back("d", "");
      }
      now.back().second = std::to_string(stage);
      database.flush();
      database.compact();
    }
  }
  reset(directory);
}

void test_released_snapshot_lets_versions_go() {
  const auto directory = test_directory("snapshot-gc");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 1 << 20;
  options.compaction_trigger = 10;
  const std::string big(1000, 'x');
  lsm::LSMTree database(directory, options);
  database.put("k", "original");
  database.put("other", "o");
  auto snapshot = database.snapshot();
  for (int version = 0; version < 100; ++version) {
    database.put("k", big + std::to_string(version));
  }
  database.flush();
  database.put("other", "o2");
  database.flush();
  database.compact();
  // Every version newer than the snapshot is kept while it lives, because
  // the newest version is not visible to it.
  std::uintmax_t held = 0;
  for (const auto& file : table_files(directory)) {
    held += std::filesystem::file_size(file);
  }
  CHECK(held > 50 * big.size());
  CHECK(database.get("k", *snapshot) == std::optional<std::string>("original"));
  snapshot.reset();
  database.put("other", "o3");
  database.flush();
  database.compact();
  std::uintmax_t released = 0;
  for (const auto& file : table_files(directory)) {
    released += std::filesystem::file_size(file);
  }
  // Once no snapshot can see them, only the newest version remains.
  CHECK(released < 3 * big.size());
  CHECK(database.get("k") == std::optional<std::string>(big + "99"));
  reset(directory);
}

void test_snapshot_survives_leveled_compaction() {
  const auto directory = test_directory("snapshot-levels");
  reset(directory);
  auto options = small_options();
  options.sync_writes = false;
  std::mt19937 random(777);
  std::map<std::string, std::optional<std::string>> then;
  std::map<std::string, std::optional<std::string>> now;
  lsm::LSMTree database(directory, options);
  for (int step = 0; step < 800; ++step) {
    const std::string key = numbered("snap-", static_cast<int>(random() % 300));
    const std::string value = "a" + std::to_string(random() % 1000);
    database.put(key, value);
    now[key] = value;
  }
  then = now;
  const auto snapshot = database.snapshot();
  for (int step = 0; step < 4000; ++step) {
    const std::string key = numbered("snap-", static_cast<int>(random() % 300));
    if (random() % 4 == 0) {
      database.erase(key);
      now[key] = std::nullopt;
    } else {
      const std::string value = "b" + std::to_string(random() % 1000);
      database.put(key, value);
      now[key] = value;
    }
  }
  database.wait_for_compactions();
  CHECK(deepest_level(database.stats()) >= 2);
  std::vector<std::pair<std::string, std::string>> expected_then;
  for (const auto& [key, value] : then) {
    CHECK(database.get(key, *snapshot) == value);
    if (value) {
      expected_then.emplace_back(key, *value);
    }
  }
  for (const auto& [key, value] : now) {
    CHECK(database.get(key) == value);
  }
  CHECK(database.scan("", "", *snapshot) == expected_then);
  reset(directory);
}

void test_tombstone_kept_while_snapshot_needs_old_value() {
  const auto directory = test_directory("snapshot-tombstone");
  reset(directory);
  lsm::Options options;
  options.compaction_trigger = 10;
  lsm::LSMTree database(directory, options);
  database.put("k", "v");
  database.flush();
  {
    const auto snapshot = database.snapshot();
    database.erase("k");
    database.flush();
    database.compact();
    CHECK(!database.get("k"));
    CHECK(database.get("k", *snapshot) == std::optional<std::string>("v"));
  }
  // Released: the tombstone and the value it hid can both go.
  database.put("z", "1");
  database.flush();
  database.compact();
  CHECK(!database.get("k"));
  CHECK(database.scan("", "").size() == 1);
  reset(directory);
}

void test_concurrent_readers_writers_and_snapshots() {
  const auto directory = test_directory("concurrent");
  reset(directory);
  auto options = small_options();
  options.memtable_bytes = 2048;
  options.compaction_trigger = 3;
  options.table_bytes = 4096;
  options.level1_bytes = 8192;
  options.sync_writes = false;
  constexpr int kWriters = 2;
  constexpr int kReaders = 4;
  constexpr int kKeys = 2500;
  std::atomic<int> progress[kWriters];
  for (auto& written : progress) {
    written = 0;
  }
  std::atomic<bool> writing{true};
  std::atomic<int> problems{0};
  std::atomic<long> reads{0};
  std::atomic<long> snapshot_checks{0};
  {
    lsm::LSMTree database(directory, options);
    std::vector<std::thread> threads;
    for (int writer = 0; writer < kWriters; ++writer) {
      threads.emplace_back([&, writer] {
        const std::string prefix = "w" + std::to_string(writer) + "-";
        for (int index = 0; index < kKeys; ++index) {
          database.put(numbered(prefix, index), std::to_string(index));
          // Published only after put() returned.
          progress[writer].store(index + 1, std::memory_order_release);
        }
      });
    }
    for (int reader = 0; reader < kReaders; ++reader) {
      threads.emplace_back([&, reader] {
        std::mt19937 random(static_cast<unsigned>(reader) + 1);
        while (writing.load()) {
          const int writer = static_cast<int>(random() % kWriters);
          const std::string prefix = "w" + std::to_string(writer) + "-";
          const int done = progress[writer].load(std::memory_order_acquire);
          if (done > 0) {
            // Every acknowledged write is visible, with its value.
            const int index = static_cast<int>(random() % done);
            if (database.get(numbered(prefix, index)) !=
                std::optional<std::string>(std::to_string(index))) {
              ++problems;
            }
            ++reads;
          }
          if (random() % 16 == 0) {
            // Keys are written in order, so a consistent snapshot holds
            // exactly keys 0..m-1 of each writer: no gaps, nothing after.
            const auto snapshot = database.snapshot();
            const int floor = done;
            const auto page = database.scan(prefix, prefix + "~", *snapshot);
            const int seen = static_cast<int>(page.size());
            if (seen < floor) {
              ++problems;
            }
            for (int index = 0; index < seen; ++index) {
              if (page[static_cast<std::size_t>(index)].first !=
                      numbered(prefix, index) ||
                  page[static_cast<std::size_t>(index)].second !=
                      std::to_string(index)) {
                ++problems;
                break;
              }
            }
            if (database.get(numbered(prefix, seen), *snapshot)) {
              ++problems;
            }
            ++snapshot_checks;
          }
        }
      });
    }
    threads.emplace_back([&] {
      for (int round = 0; round < 6 && writing.load(); ++round) {
        database.flush();
        database.compact();
      }
    });
    for (int writer = 0; writer < kWriters; ++writer) {
      threads[static_cast<std::size_t>(writer)].join();
    }
    writing = false;
    for (std::size_t index = kWriters; index < threads.size(); ++index) {
      threads[index].join();
    }
    database.wait_for_compactions();
    CHECK(problems.load() == 0);
    CHECK(reads.load() > 1000);
    CHECK(snapshot_checks.load() > 50);
    CHECK(database.stats().compaction_bytes_written > 0);
  }
  lsm::LSMTree database(directory, options);
  for (int writer = 0; writer < kWriters; ++writer) {
    const std::string prefix = "w" + std::to_string(writer) + "-";
    CHECK(database.scan(prefix, prefix + "~").size() ==
          static_cast<std::size_t>(kKeys));
  }
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
  test_data_moves_through_levels();
  test_manifest_decides_which_tables_are_live();
  test_directory_without_manifest_is_migrated();
  test_damaged_manifest_is_rejected();
  test_manifest_appends_edits();
  test_manifest_log_stays_bounded();
  test_torn_manifest_tail_is_ignored();
  test_corrupt_manifest_record_before_tail_fails_open();
  test_version1_manifest_is_upgraded();
  test_version2_table_is_read_and_migrated();
  test_prefix_compression_shrinks_tables();
  test_restart_points_find_every_key();
  test_snapshot_sees_a_frozen_view();
  test_released_snapshot_lets_versions_go();
  test_snapshot_survives_leveled_compaction();
  test_tombstone_kept_while_snapshot_needs_old_value();
  test_concurrent_readers_writers_and_snapshots();
  if (failures != 0) {
    std::cerr << failures << " of " << assertions << " assertions failed\n";
    return 1;
  }
  std::cout << assertions << " assertions passed\n";
  return 0;
}
