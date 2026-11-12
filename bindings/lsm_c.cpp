#include "lsm_c.h"

#include "lsm/lsm.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

struct lsm_db {
  lsm::LSMTree tree;

  lsm_db(const char* path, const lsm::Options& options)
      : tree(path, options) {}
};

namespace {

char* copy_bytes(const char* data, std::size_t size) {
  // Allocate at least one byte so an empty value is still a valid pointer.
  auto* buffer = static_cast<char*>(std::malloc(size == 0 ? 1 : size));
  if (buffer == nullptr) {
    throw std::bad_alloc();
  }
  if (size != 0) {
    std::memcpy(buffer, data, size);
  }
  return buffer;
}

void set_error(char** error, const char* message) {
  if (error == nullptr) {
    return;
  }
  const std::size_t length = std::strlen(message);
  *error = static_cast<char*>(std::malloc(length + 1));
  if (*error != nullptr) {
    std::memcpy(*error, message, length + 1);
  }
}

// Runs `body` and converts any C++ exception into a status code.
template <typename Body>
int guarded(char** error, Body&& body) {
  if (error != nullptr) {
    *error = nullptr;
  }
  try {
    return body();
  } catch (const std::invalid_argument& exception) {
    set_error(error, exception.what());
    return LSM_INVALID_ARGUMENT;
  } catch (const std::exception& exception) {
    set_error(error, exception.what());
    return LSM_ERROR;
  } catch (...) {
    set_error(error, "unknown C++ exception");
    return LSM_ERROR;
  }
}

void append_u32(std::vector<char>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::vector<char>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

}  // namespace

extern "C" {

int lsm_open(const char* path, uint64_t memtable_bytes,
             uint64_t bloom_bits_per_key, uint64_t compaction_trigger,
             int sync_writes, lsm_db** out, char** error) {
  return guarded(error, [&] {
    if (path == nullptr || out == nullptr) {
      throw std::invalid_argument("path and out must not be null");
    }
    lsm::Options options;
    options.memtable_bytes = static_cast<std::size_t>(memtable_bytes);
    options.bloom_bits_per_key = static_cast<std::size_t>(bloom_bits_per_key);
    options.compaction_trigger = static_cast<std::size_t>(compaction_trigger);
    options.sync_writes = sync_writes != 0;
    *out = new lsm_db(path, options);
    return LSM_OK;
  });
}

void lsm_close(lsm_db* db) { delete db; }

int lsm_put(lsm_db* db, const char* key, uint64_t key_len, const char* value,
            uint64_t value_len, char** error) {
  return guarded(error, [&] {
    db->tree.put(std::string(key, key_len), std::string(value, value_len));
    return LSM_OK;
  });
}

int lsm_delete(lsm_db* db, const char* key, uint64_t key_len, char** error) {
  return guarded(error, [&] {
    db->tree.erase(std::string(key, key_len));
    return LSM_OK;
  });
}

int lsm_get(lsm_db* db, const char* key, uint64_t key_len, char** value,
            uint64_t* value_len, char** error) {
  return guarded(error, [&] {
    *value = nullptr;
    *value_len = 0;
    const auto found = db->tree.get(std::string(key, key_len));
    if (!found) {
      return static_cast<int>(LSM_NOT_FOUND);
    }
    *value = copy_bytes(found->data(), found->size());
    *value_len = found->size();
    return static_cast<int>(LSM_OK);
  });
}

int lsm_scan(lsm_db* db, const char* begin, uint64_t begin_len,
             const char* end, uint64_t end_len, char** out, uint64_t* out_len,
             char** error) {
  return guarded(error, [&] {
    *out = nullptr;
    *out_len = 0;
    const auto pairs = db->tree.scan(std::string(begin, begin_len),
                                     std::string(end, end_len));
    std::vector<char> buffer;
    append_u64(buffer, pairs.size());
    for (const auto& [key, value] : pairs) {
      append_u32(buffer, static_cast<std::uint32_t>(key.size()));
      buffer.insert(buffer.end(), key.begin(), key.end());
      append_u32(buffer, static_cast<std::uint32_t>(value.size()));
      buffer.insert(buffer.end(), value.begin(), value.end());
    }
    *out = copy_bytes(buffer.data(), buffer.size());
    *out_len = buffer.size();
    return LSM_OK;
  });
}

int lsm_flush(lsm_db* db, char** error) {
  return guarded(error, [&] {
    db->tree.flush();
    return LSM_OK;
  });
}

int lsm_compact(lsm_db* db, char** error) {
  return guarded(error, [&] {
    db->tree.compact();
    return LSM_OK;
  });
}

void lsm_get_stats(lsm_db* db, lsm_stats* out) {
  const lsm::Stats stats = db->tree.stats();
  out->memtable_entries = stats.memtable_entries;
  out->sstable_count = stats.sstable_count;
  out->sequence = stats.sequence;
  out->bloom_checks = stats.bloom_checks;
  out->bloom_negative_hits = stats.bloom_negative_hits;
}

void lsm_free(void* pointer) { std::free(pointer); }

}  // extern "C"
