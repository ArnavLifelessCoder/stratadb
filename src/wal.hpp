#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "memtable.hpp"
#include "platform/file.hpp"
#include "write_batch.hpp"

namespace lsm::internal {

class Wal {
 public:
  explicit Wal(std::unique_ptr<platform::File> file) : file_(std::move(file)) {}
  auto Append(const WriteBatch& batch, std::uint64_t sequence, bool sync) -> Status;
  auto Replay(MemTable& table, std::uint64_t& last_sequence, std::uint64_t first_sequence = 1)
      -> Status;
  auto Sync() -> Status;
  [[nodiscard]] auto BytesWritten() const -> std::uint64_t { return bytes_written_; }
  [[nodiscard]] auto SyncCount() const -> std::uint64_t { return sync_count_; }
  [[nodiscard]] auto TruncatedBytes() const -> std::uint64_t { return truncated_bytes_; }

 private:
  auto RepairTail(std::size_t offset, std::size_t size) -> Status;
  std::unique_ptr<platform::File> file_;
  Status failure_;
  std::uint64_t bytes_written_ = 0;
  std::uint64_t sync_count_ = 0;
  std::uint64_t truncated_bytes_ = 0;
};

}  // namespace lsm::internal
