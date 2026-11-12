# Storage contract

## Write path

Each write takes the next sequence number, appends a length-delimited,
CRC32-protected record to the current WAL file (`wal-N.log`), and is
acknowledged only after `fsync`. It then enters the memtable, keyed by
(key, sequence) with versions of a key ordered newest first. Without live
snapshots an older version is replaced in place; while a snapshot exists it
is kept.

Each memtable has its own WAL file, created and made durable (directory
fsync) before any write in it is acknowledged. When the memtable is full it
is frozen and handed to the background thread, and writes continue into a
new memtable and a new WAL. A writer waits only if the previous frozen
memtable is still being flushed.

Writers take turns on a writer lock, so their fsyncs never block readers.

## Flush and compaction

One background thread does every flush, compaction and `MANIFEST` commit.

A flush streams the frozen memtable in key order into a temporary SSTable of
checksummed, prefix-compressed data blocks with restart points, followed by a
block index, the Bloom filter and a footer. The table is synced, renamed and
its directory synced; one `MANIFEST` edit naming it is appended and fsynced,
and only then is the frozen memtable's WAL deleted.

Level 0 compacts into level 1 once it holds `compaction_trigger` tables: all
level 0 tables merge with every level 1 table their combined range overlaps.
A deeper level compacts once its bytes exceed `level1_bytes` times
`level_multiplier` to the power (level - 1): one table, chosen round-robin
through the key space, merges with the overlapping tables one level down, or
moves down by a `MANIFEST` edit alone if none overlap. Output is split into
tables of about `table_bytes`, only ever between different keys.

Both keep a version unless a newer version of the same key is visible to
every reader: the oldest live snapshot, or the latest sequence when there is
none. A tombstone is dropped only if no snapshot predates it and no table in
a level below the output overlaps its key. Outputs are synced and renamed,
one `MANIFEST` edit replaces the inputs with them, and only then are input
files deleted.

## Read path

A read returns the newest version of a key whose sequence is at most its
bound -- the latest sequence, or a snapshot's. It checks:

1. the live memtable, under the database lock;
2. the frozen memtable, if any, pinned under that lock;
3. level 0 tables, newest generation first;
4. levels 1 to 6, at most one table per level: the one whose key range
   contains the key, found by binary search.

Steps 2-4 use a table set pinned under the lock and run without it. Shallower
levels always hold newer data, so the first visible version found wins. A
Bloom filter can prove absence; otherwise the block index locates one data
block, which is checksum-verified, cached in a sharded LRU cache, and
searched through its restart points. Tombstones terminate the search.

## Recovery

Opening the database reads the `MANIFEST`, which alone decides which tables
are live and at which level. It replays the snapshot record and every edit;
an incomplete or checksum-failing final record is an uncommitted edit and is
dropped, but a damaged earlier record fails the open. Table files it does not
list come from a flush or compaction that crashed before committing, and are
deleted. A missing listed table, or overlapping tables in a level of 1 or
more, also fails the open. A directory with no `MANIFEST` predates levels:
every table becomes level 0 in generation order.

Every WAL file is then replayed in order (a legacy `active.wal` first), each
truncated at its first incomplete or corrupt record, and a new WAL is started
for new writes. Sequence numbers impose a total last-write-wins order, and
the same version found in a WAL and a table merges as one.
