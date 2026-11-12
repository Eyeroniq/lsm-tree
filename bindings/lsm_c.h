/*
 * C ABI for the LSM engine, used by the Java service through the Foreign
 * Function & Memory API. C++ exceptions never cross this boundary: every
 * call returns a status code, and failures can hand back an error message.
 *
 * Ownership: any buffer returned through an out-parameter (values, scan
 * results, error messages) is allocated here and must be released with
 * lsm_free. An lsm_db handle is not thread-safe; callers serialize access.
 */
#ifndef LSM_C_H
#define LSM_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lsm_db lsm_db;

enum {
  LSM_OK = 0,
  LSM_NOT_FOUND = 1,
  LSM_INVALID_ARGUMENT = 2,
  LSM_ERROR = 3
};

#define LSM_MAX_LEVELS 7

typedef struct {
  uint64_t memtable_entries;
  uint64_t sstable_count;
  uint64_t sequence;
  uint64_t bloom_checks;
  uint64_t bloom_negative_hits;
  uint64_t block_reads;
  uint64_t block_cache_hits;
  uint64_t compaction_bytes_written;
  uint64_t tables_per_level[LSM_MAX_LEVELS];
} lsm_stats;

/* Opens (or creates) the database in `path`, a NUL-terminated string. */
/* table_bytes and level1_bytes size leveled compaction; 0 keeps the
 * engine's default. */
int lsm_open(const char* path, uint64_t memtable_bytes,
             uint64_t bloom_bits_per_key, uint64_t compaction_trigger,
             uint64_t table_bytes, uint64_t level1_bytes, int sync_writes,
             lsm_db** out, char** error);
void lsm_close(lsm_db* db);

int lsm_put(lsm_db* db, const char* key, uint64_t key_len, const char* value,
            uint64_t value_len, char** error);
int lsm_delete(lsm_db* db, const char* key, uint64_t key_len, char** error);

/* Returns LSM_OK with a copy of the value, or LSM_NOT_FOUND. */
int lsm_get(lsm_db* db, const char* key, uint64_t key_len, char** value,
            uint64_t* value_len, char** error);

/*
 * Range scan over [begin, end); an empty `end` means no upper bound.
 * The result buffer is little-endian:
 *   u64 count, then per pair: u32 key_len, key, u32 value_len, value.
 */
int lsm_scan(lsm_db* db, const char* begin, uint64_t begin_len,
             const char* end, uint64_t end_len, char** out, uint64_t* out_len,
             char** error);

int lsm_flush(lsm_db* db, char** error);
int lsm_compact(lsm_db* db, char** error);
void lsm_get_stats(lsm_db* db, lsm_stats* out);

void lsm_free(void* pointer);

#ifdef __cplusplus
}
#endif

#endif /* LSM_C_H */
