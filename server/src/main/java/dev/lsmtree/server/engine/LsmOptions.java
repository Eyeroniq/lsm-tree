package dev.lsmtree.server.engine;

/** Engine tuning, mirroring lsm::Options in include/lsm/lsm.h. */
public record LsmOptions(
    long memtableBytes, long bloomBitsPerKey, long compactionTrigger, boolean syncWrites) {

  public static LsmOptions defaults() {
    return new LsmOptions(64 * 1024, 10, 4, true);
  }
}
