#include "lsm/lsm.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
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
  if (failures != 0) {
    std::cerr << failures << " of " << assertions << " assertions failed\n";
    return 1;
  }
  std::cout << assertions << " assertions passed\n";
  return 0;
}
