#include "lsm/lsm.h"

#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <unistd.h>

namespace {

void repair_oracle(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return;
  }
  std::ifstream input(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  if (bytes.empty() || bytes.back() == '\n') {
    return;
  }
  const auto newline = bytes.find_last_of('\n');
  std::filesystem::resize_file(
      path, newline == std::string::npos ? 0 : newline + 1);
}

void append_oracle(const std::filesystem::path& path, const std::string& key,
                   const std::string& value) {
  const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd < 0) {
    throw std::runtime_error("failed to open oracle");
  }
  const std::string line = key + "\t" + value + "\n";
  const ssize_t count = ::write(fd, line.data(), line.size());
  if (count != static_cast<ssize_t>(line.size()) || ::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("failed to persist oracle");
  }
  ::close(fd);
}

int workload(const std::filesystem::path& directory,
             const std::filesystem::path& oracle) {
  repair_oracle(oracle);
  lsm::Options options;
  options.memtable_bytes = 512;
  options.compaction_trigger = 3;
  lsm::LSMTree database(directory, options);
  for (;;) {
    const std::uint64_t id = database.stats().sequence + 1;
    const std::string key = "crash-key-" + std::to_string(id);
    const std::string value = "crash-value-" + std::to_string(id);
    database.put(key, value);
    append_oracle(oracle, key, value);
  }
}

int verify(const std::filesystem::path& directory,
           const std::filesystem::path& oracle) {
  repair_oracle(oracle);
  lsm::Options options;
  options.memtable_bytes = 512;
  options.compaction_trigger = 3;
  lsm::LSMTree database(directory, options);
  std::ifstream input(oracle);
  std::string line;
  std::size_t acknowledged = 0;
  while (std::getline(input, line)) {
    const auto separator = line.find('\t');
    if (separator == std::string::npos) {
      break;
    }
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (database.get(key) != std::optional<std::string>(value)) {
      std::cerr << "lost acknowledged write: " << key << "\n";
      return 1;
    }
    ++acknowledged;
  }
  const auto stats = database.stats();
  std::cout << "acknowledged=" << acknowledged
            << " sequence=" << stats.sequence
            << " sstables=" << stats.sstable_count << "\n";
  return 0;
}

int benchmark(const std::filesystem::path& directory, std::size_t count) {
  std::filesystem::remove_all(directory);
  lsm::Options options;
  options.memtable_bytes = 256 * 1024;
  options.compaction_trigger = 4;

  const auto write_started = std::chrono::steady_clock::now();
  {
    lsm::LSMTree database(directory, options);
    for (std::size_t index = 0; index < count; ++index) {
      database.put("key-" + std::to_string(index),
                   "value-" + std::to_string(index));
    }
    database.flush();
  }
  const auto write_finished = std::chrono::steady_clock::now();

  std::size_t found = 0;
  const auto read_started = std::chrono::steady_clock::now();
  lsm::LSMTree database(directory, options);
  std::mt19937_64 random(20260728);
  std::uniform_int_distribution<std::size_t> distribution(0, count - 1);
  for (std::size_t index = 0; index < count; ++index) {
    if (database.get("key-" + std::to_string(distribution(random)))) {
      ++found;
    }
  }
  const auto read_finished = std::chrono::steady_clock::now();

  const double write_seconds =
      std::chrono::duration<double>(write_finished - write_started).count();
  const double read_seconds =
      std::chrono::duration<double>(read_finished - read_started).count();
  const auto stats = database.stats();
  std::cout << "writes=" << count
            << " write_ops_per_second=" << count / write_seconds
            << " reads=" << count << " read_ops_per_second="
            << count / read_seconds << " found=" << found
            << " sstables=" << stats.sstable_count
            << " bloom_checks=" << stats.bloom_checks << "\n";
  return found == count ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 4 && std::string(argv[1]) == "workload") {
      return workload(argv[2], argv[3]);
    }
    if (argc == 4 && std::string(argv[1]) == "verify") {
      return verify(argv[2], argv[3]);
    }
    if (argc == 4 && std::string(argv[1]) == "benchmark") {
      const std::size_t count = std::stoull(argv[3]);
      if (count == 0) {
        throw std::invalid_argument("operation count must be positive");
      }
      return benchmark(argv[2], count);
    }
    std::cerr
        << "usage:\n"
        << "  lsm_tool workload <directory> <oracle>\n"
        << "  lsm_tool verify <directory> <oracle>\n"
        << "  lsm_tool benchmark <directory> <operation-count>\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
