package dev.lsmtree.server.engine;

/**
 * Engine tuning, mirroring lsm::Options in include/lsm/lsm.h. A table or
 * level-1 size of 0 keeps the engine's default.
 */
public record LsmOptions(
    long memtableBytes,
    long bloomBitsPerKey,
    long compactionTrigger,
    long tableBytes,
    long level1Bytes,
    boolean syncWrites) {

  public LsmOptions(
      long memtableBytes, long bloomBitsPerKey, long compactionTrigger, boolean syncWrites) {
    this(memtableBytes, bloomBitsPerKey, compactionTrigger, 0, 0, syncWrites);
  }

  public static LsmOptions defaults() {
    return new LsmOptions(64 * 1024, 10, 4, true);
  }
}
