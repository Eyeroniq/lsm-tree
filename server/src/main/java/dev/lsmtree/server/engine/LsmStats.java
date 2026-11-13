package dev.lsmtree.server.engine;

/** Snapshot of the engine's counters, mirroring lsm::Stats. */
public record LsmStats(
    long memtableEntries,
    long sstableCount,
    long sequence,
    long bloomChecks,
    long bloomNegativeHits,
    long blockReads,
    long blockCacheHits,
    long compactionBytesWritten,
    long[] tablesPerLevel) {

  /** Levels reported by the engine (L0 to L6). */
  public static final int MAX_LEVELS = 7;
}
