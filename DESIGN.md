# Storage contract

## Write path

`put` and `erase` first append a length-delimited, CRC32-protected record to the
active WAL. The call is acknowledged only after `fsync`. The operation then
updates the ordered in-memory table.

When the memtable crosses its configured limit, it is written in key order to a
temporary SSTable. The table is synced, atomically renamed, and its directory is
synced before the WAL is reset.

## Read path

Reads check:

1. the mutable memtable;
2. immutable SSTables from newest generation to oldest.

A table Bloom filter can prove absence. A positive result is followed by an
index lookup and a checksummed record read. Tombstones terminate the search.

## Recovery and compaction

Opening the database discovers valid SSTables and replays the verified WAL
prefix. Incomplete or corrupt WAL tails are truncated. Sequence numbers impose
a total last-write-wins order.

Compaction merges tables into a newer generation and retains tombstones. The
new table is durable before old generations are removed, so a crash during
cleanup cannot resurrect a deleted value.
