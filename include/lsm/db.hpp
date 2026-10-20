#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/status.hpp"
#include "lsm/write_batch.hpp"

namespace lsm {

class Snapshot;

struct Options {
  std::size_t memtable_bytes = 4 << 20;
  std::size_t block_bytes = 4096;
  int bloom_bits_per_key = 10;
  std::size_t block_cache_bytes = 64 << 20;
  int l0_compaction_trigger = 4;
  int l0_stop_writes = 12;
  double level_size_ratio = 10.0;
};
struct WriteOptions {
  bool sync = false;
};
struct ReadOptions {
  const Snapshot* snapshot = nullptr;
};
struct Stats {
  // Write counters cover this Open session, excluding replay.
  std::uint64_t user_bytes_written = 0;
  std::uint64_t disk_bytes_written = 0;
  std::vector<std::uint64_t> level_file_counts;
  std::uint64_t wal_syncs = 0;
  std::uint64_t recovered_tail_bytes = 0;
  std::size_t memtable_bytes = 0;
};

// Concurrent reads and writes are supported. Stop callers before destruction.
// One process/instance may own a database directory. Errors leave Get's value
// and Open's out unchanged. Keys and values may contain any bytes, including NUL.
class DB {
 public:
  static Status Open(const Options&, const std::filesystem::path&, std::unique_ptr<DB>* out);
  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  Status Put(const WriteOptions&, std::string_view key, std::string_view value);
  Status Delete(const WriteOptions&, std::string_view key);
  Status Write(const WriteOptions&, WriteBatch* batch);
  Status Get(const ReadOptions&, std::string_view key, std::string* value);
  Stats GetStats();

 private:
  struct Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace lsm
