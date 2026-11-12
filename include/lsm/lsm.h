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
  // Number of freshly flushed (level 0) tables that triggers a compaction
  // into level 1.
  std::size_t compaction_trigger = 4;
  // Leveled compaction: level 1 may hold level1_bytes, and each deeper level
  // level_multiplier times more. Compaction output is split into tables of
  // about table_bytes.
  std::size_t table_bytes = 2 * 1024 * 1024;
  std::size_t level1_bytes = 10 * 1024 * 1024;
  std::size_t level_multiplier = 10;
  // Target size of an SSTable data block. Only one key per block is kept in
  // memory; a point read loads a single block from disk.
  std::size_t block_bytes = 4096;
  // Budget for decoded data blocks kept in memory (LRU). 0 disables caching.
  std::size_t block_cache_bytes = 8 * 1024 * 1024;
  bool sync_writes = true;
};

struct Stats {
  std::size_t memtable_entries = 0;
  std::size_t sstable_count = 0;
  std::uint64_t sequence = 0;
  std::uint64_t bloom_checks = 0;
  std::uint64_t bloom_negative_hits = 0;
  // SSTable data blocks read from disk (and checksum-verified).
  std::uint64_t block_reads = 0;
  // Block lookups served from the block cache instead of disk.
  std::uint64_t block_cache_hits = 0;
  // Bytes of SSTable written by compaction since open (write amplification).
  std::uint64_t compaction_bytes_written = 0;
  // Number of tables in each level, level 0 first.
  std::vector<std::size_t> tables_per_level;
};

class LSMTree;

// A consistent point-in-time view. Reads through a snapshot see exactly the
// writes acknowledged before it was taken, however much is written,
// flushed or compacted afterwards. Versions it can see are kept until it is
// released (destroyed).
class Snapshot {
 public:
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  ~Snapshot();

  // Sequence number of the last write the snapshot sees.
  std::uint64_t sequence() const { return sequence_; }

 private:
  friend class LSMTree;
  struct Registry;
  Snapshot(std::uint64_t sequence, std::shared_ptr<Registry> registry);

  std::uint64_t sequence_;
  std::shared_ptr<Registry> registry_;
};

class LSMTree {
 public:
  struct Entry;
  struct Table;
  struct BlockCache;

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

  std::shared_ptr<const Snapshot> snapshot() const;
  std::optional<std::string> get(const std::string& key,
                                 const Snapshot& snapshot) const;
  std::vector<std::pair<std::string, std::string>> scan(
      const std::string& begin, const std::string& end,
      const Snapshot& snapshot) const;

  void flush();
  void compact();
  Stats stats() const;

 private:
  // levels[0] holds overlapping tables, newest first; deeper levels hold
  // tables with disjoint key ranges, sorted by smallest key.
  using Levels = std::vector<std::vector<std::shared_ptr<Table>>>;

  // Memtable keys are (key, sequence), ordered by key and then newest first.
  struct VersionOrder {
    bool operator()(const std::pair<std::string, std::uint64_t>& left,
                    const std::pair<std::string, std::uint64_t>& right) const {
      return left.first < right.first ||
             (left.first == right.first && left.second > right.second);
    }
  };

  void mutate(const std::string& key, const std::string& value,
              bool tombstone);
  void insert_memtable(const std::string& key, Entry entry);
  std::optional<std::string> read(const std::string& key,
                                  std::uint64_t at) const;
  std::vector<std::pair<std::string, std::string>> scan_at(
      const std::string& begin, const std::string& end,
      std::uint64_t at) const;
  std::uint64_t smallest_snapshot() const;
  void acquire_lock();
  void open_tables();
  void commit(Levels next);
  void rewrite_manifest();
  void recover_wal();
  std::filesystem::path table_path(std::uint64_t generation,
                                   bool temporary) const;
  void maybe_compact();
  void compact_level0();
  void compact_level(std::size_t level);
  void run_compaction(const std::vector<std::shared_ptr<Table>>& inputs,
                      std::size_t output_level);
  bool is_base_level(std::size_t level, const std::string& key) const;
  std::uint64_t level_limit(std::size_t level) const;
  void reset_wal();
  void sync_directory() const;
  void require_writable() const;

  std::filesystem::path directory_;
  std::filesystem::path wal_path_;
  Options options_;
  int wal_fd_ = -1;
  // Holds an exclusive flock on <directory>/LOCK for the object's lifetime.
  int lock_fd_ = -1;
  // Append-only MANIFEST log: open for appending edits, its size, and the
  // size of the snapshot it starts with (to decide when to rewrite it).
  int manifest_fd_ = -1;
  std::uint64_t manifest_bytes_ = 0;
  std::uint64_t manifest_snapshot_bytes_ = 0;
  // Byte length of the WAL up to the end of its last complete record.
  std::uint64_t wal_size_ = 0;
  // Set when an I/O error leaves the on-disk state uncertain; all further
  // mutations are refused until the database is reopened.
  bool failed_ = false;
  std::uint64_t sequence_ = 0;
  std::uint64_t next_generation_ = 1;
  std::size_t memtable_size_ = 0;
  std::map<std::pair<std::string, std::uint64_t>, Entry, VersionOrder>
      memtable_;
  std::shared_ptr<Snapshot::Registry> snapshots_;
  Levels levels_;
  // Per level, the largest key of the last table compacted from it, so that
  // successive compactions walk the key space round-robin.
  std::vector<std::string> compact_pointer_;
  std::uint64_t compaction_bytes_written_ = 0;
  mutable std::uint64_t bloom_checks_ = 0;
  mutable std::uint64_t bloom_negative_hits_ = 0;
  mutable std::uint64_t block_reads_ = 0;
  mutable std::unique_ptr<BlockCache> block_cache_;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

}  // namespace lsm
