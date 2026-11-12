package dev.lsmtree.server.engine;

/** Snapshot of the engine's counters, mirroring lsm::Stats. */
public record LsmStats(
    long memtableEntries,
    long sstableCount,
    long sequence,
    long bloomChecks,
    long bloomNegativeHits) {}
