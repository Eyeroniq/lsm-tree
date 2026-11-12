# Storage contract

## Write path

`put` and `erase` first append a length-delimited, CRC32-protected record to the
active WAL. The call is acknowledged only after `fsync`. The operation then
updates the ordered in-memory table.

When the memtable crosses its configured limit, it is streamed in key order
into a temporary SSTable of checksummed data blocks, followed by a block
index, the Bloom filter and a footer. The table is synced, atomically renamed,
and its directory is synced before the WAL is reset.

## Read path

Reads check:

1. the mutable memtable;
2. immutable SSTables from newest generation to oldest.

A table Bloom filter can prove absence. A positive result is followed by a
binary search of the in-memory block index and a read of one data block, which
is checksum-verified before use and then kept in an LRU block cache. Tombstones
terminate the search.

Only the block index (one key per block) and the Bloom filter of each table
are held in memory. Tables in the older version 1 format are still loaded
whole.

## Recovery and compaction

Opening the database discovers valid SSTables and replays the verified WAL
prefix. Incomplete or corrupt WAL tails are truncated. Sequence numbers impose
a total last-write-wins order.

Compaction merges tables into a newer generation and retains tombstones. It
streams a k-way merge over per-table cursors, holding one block per source,
and bypasses the block cache. The new table is durable before old generations
are removed, so a crash during cleanup cannot resurrect a deleted value.
Compaction always writes version 2, which migrates version 1 tables.
