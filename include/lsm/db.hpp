#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/iterator.hpp"
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
  // Table bytes per level, same indexing as level_file_counts.
  std::vector<std::uint64_t> level_bytes;
  // Background work completed during this Open session.
  std::uint64_t flushes = 0;
  std::uint64_t compactions = 0;
  std::uint64_t wal_syncs = 0;
  std::uint64_t recovered_tail_bytes = 0;
  std::size_t memtable_bytes = 0;
  // Group commit: user write calls that reached the WAL, and the WAL records
  // they were combined into (one per leader group).
  std::uint64_t write_calls = 0;
  std::uint64_t wal_records = 0;
  // Time writers spent waiting on L0 slowdown/stop or a full immutable memtable.
  std::uint64_t stall_micros = 0;
  std::uint64_t slowed_writes = 0;
  std::uint64_t stopped_writes = 0;
  // Data-block cache.
  std::uint64_t block_cache_hits = 0;
  std::uint64_t block_cache_misses = 0;
  std::uint64_t block_cache_bytes = 0;
};

// Concurrent reads and writes are supported. Stop callers before destruction.
// One process/instance may own a database directory. Errors leave Get's value
// and Open's out unchanged. Keys and values may contain any bytes, including NUL.
class DB {
 public:
  static auto Open(const Options&, const std::filesystem::path&, std::unique_ptr<DB>* out)
      -> Status;
  ~DB();
  DB(const DB&) = delete;
  auto operator=(const DB&) -> DB& = delete;

  auto Put(const WriteOptions&, std::string_view key, std::string_view value) -> Status;
  auto Delete(const WriteOptions&, std::string_view key) -> Status;
  auto Write(const WriteOptions&, WriteBatch* batch) -> Status;
  auto Get(const ReadOptions&, std::string_view key, std::string* value) -> Status;
  // Iterators capture a stable read view. Release all snapshots and iterators
  // before destroying this DB. A snapshot belongs only to the DB that created it.
  auto NewIterator(const ReadOptions&) -> std::unique_ptr<Iterator>;
  auto GetSnapshot() -> const Snapshot*;
  void ReleaseSnapshot(const Snapshot*);
  auto GetStats() -> Stats;

 private:
  struct Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace lsm
