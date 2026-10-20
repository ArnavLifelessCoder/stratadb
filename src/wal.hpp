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
  Status Append(const WriteBatch& batch, std::uint64_t sequence, bool sync);
  Status Replay(MemTable& table, std::uint64_t& last_sequence);
  [[nodiscard]] std::uint64_t BytesWritten() const { return bytes_written_; }
  [[nodiscard]] std::uint64_t SyncCount() const { return sync_count_; }
  [[nodiscard]] std::uint64_t TruncatedBytes() const { return truncated_bytes_; }

 private:
  Status RepairTail(std::size_t offset, std::size_t size);
  std::unique_ptr<platform::File> file_;
  Status failure_;
  std::uint64_t bytes_written_ = 0;
  std::uint64_t sync_count_ = 0;
  std::uint64_t truncated_bytes_ = 0;
};

}  // namespace lsm::internal
