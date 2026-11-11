#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lsm {

struct Options {
  std::size_t memtable_bytes = 64 * 1024;
  std::size_t bloom_bits_per_key = 10;
  std::size_t compaction_trigger = 4;
  bool sync_writes = true;
};

struct Stats {
  std::size_t memtable_entries = 0;
  std::size_t sstable_count = 0;
  std::uint64_t sequence = 0;
  std::uint64_t bloom_checks = 0;
  std::uint64_t bloom_negative_hits = 0;
};

class LSMTree {
 public:
  struct Entry;
  struct Table;

  explicit LSMTree(std::filesystem::path directory,
                   Options options = Options{});
  ~LSMTree();

  LSMTree(const LSMTree&) = delete;
  LSMTree& operator=(const LSMTree&) = delete;

  void put(const std::string& key, const std::string& value);
  void erase(const std::string& key);
  std::optional<std::string> get(const std::string& key) const;
  std::vector<std::pair<std::string, std::string>> scan(
      const std::string& begin, const std::string& end) const;

  void flush();
  void compact();
  Stats stats() const;

 private:
  void mutate(const std::string& key, const std::string& value,
              bool tombstone);
  void discover_tables();
  void recover_wal();
  std::shared_ptr<Table> write_table(
      const std::map<std::string, Entry>& entries);
  void reset_wal();
  void sync_directory() const;
  void require_writable() const;

  std::filesystem::path directory_;
  std::filesystem::path wal_path_;
  Options options_;
  int wal_fd_ = -1;
  // Byte length of the WAL up to the end of its last complete record.
  std::uint64_t wal_size_ = 0;
  // Set when an I/O error leaves the on-disk state uncertain; all further
  // mutations are refused until the database is reopened.
  bool failed_ = false;
  std::uint64_t sequence_ = 0;
  std::uint64_t next_generation_ = 1;
  std::size_t memtable_size_ = 0;
  std::map<std::string, Entry> memtable_;
  std::vector<std::shared_ptr<Table>> tables_;
  mutable std::uint64_t bloom_checks_ = 0;
  mutable std::uint64_t bloom_negative_hits_ = 0;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

}  // namespace lsm
