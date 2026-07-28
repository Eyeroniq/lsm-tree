# lsm-tree

A dependency-free C++17 log-structured merge-tree storage engine built from
first principles.

The project targets the parts that make an LSM tree interesting:

- acknowledged writes survive process crashes through a checksummed WAL;
- sorted memtables flush to immutable, checksummed SSTables;
- Bloom filters reject absent keys before table lookup;
- newest-version-wins reads span memory and multiple disk generations;
- tombstones prevent deleted values from resurfacing;
- compaction merges generations without breaking crash recovery.

The README will be updated with measured correctness and performance results
when the implementation milestone passes on macOS and Linux.

## Intended verification

```bash
make test
make benchmark
```

No database, serialization library, checksum package, Bloom-filter package, or
test framework is hidden behind the implementation.
