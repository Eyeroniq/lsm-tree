# Storage contract

## Write path

`put` and `erase` first append a length-delimited, CRC32-protected record to the
active WAL. The call is acknowledged only after `fsync`. The operation then
updates the ordered in-memory table.

When the memtable crosses its configured limit, it is streamed in key order
into a temporary SSTable of checksummed data blocks, followed by a block
index, the Bloom filter and a footer. The table is synced, atomically renamed
and its directory synced. A new `MANIFEST` listing it as a level 0 table is
then committed (written, synced, renamed, directory synced), and only after
that is the WAL reset.

## Read path

Reads check:

1. the mutable memtable;
2. level 0 tables, newest generation first;
3. levels 1 to 6 in order, at most one table per level: the one whose key
   range contains the key, found by binary search.

Shallower levels always hold newer data, so the first version found wins.
A table Bloom filter can prove absence. A positive result is followed by a
binary search of the in-memory block index and a read of one data block, which
is checksum-verified before use and then kept in an LRU block cache. Tombstones
terminate the search.

Only the block index (one key per block) and the Bloom filter of each table
are held in memory. Tables in the older version 1 format are still loaded
whole.

## Recovery

Opening the database reads the `MANIFEST`, which alone decides which tables
are live and at which level. Table files it does not list come from a flush
or compaction that crashed before committing, and are deleted. A table it
lists but that is missing, a damaged `MANIFEST`, or overlapping tables in a
level of 1 or more all fail the open. A directory with no `MANIFEST` predates
levels; every table becomes level 0 in generation order and a `MANIFEST` is
written. The verified WAL prefix is then replayed. Incomplete or corrupt WAL
tails are truncated. Sequence numbers impose a total last-write-wins order.

## Compaction

Level 0 compacts into level 1 once it holds `compaction_trigger` tables: all
level 0 tables merge with every level 1 table their combined range overlaps.
A deeper level compacts once its bytes exceed `level1_bytes` times
`level_multiplier` to the power (level - 1): one table, chosen round-robin
through the key space, merges with the overlapping tables one level down, or
moves down by a `MANIFEST` update alone if none overlap. Output is split into
tables of about `table_bytes`.

A compaction streams a k-way merge over per-table cursors, holding one block
per input and bypassing the block cache, and keeps only the newest version of
each key. A tombstone is dropped only if no table in a level below the output
overlaps its key. Outputs are synced and renamed, then a single `MANIFEST`
commit replaces the inputs with them, and only then are input files deleted.
At no point are both, or neither, live. Compaction always writes version 2
tables, which migrates version 1 tables.
