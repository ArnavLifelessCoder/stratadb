#include "lsm/db.hpp"

#include <atomic>
#include <mutex>
#include <system_error>
#include <utility>

#include "memtable.hpp"
#include "platform/file.hpp"
#include "wal.hpp"

namespace lsm {

struct DB::Impl {
  // The lock outlives the WAL, memtable, and all other DB state.
  std::unique_ptr<platform::File> lock;
  std::unique_ptr<internal::Wal> wal;
  internal::MemTable table;
  std::mutex writer;
  std::atomic<std::uint64_t> sequence{0};
  std::uint64_t user_bytes = 0;
  Status failure;
};

DB::DB(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DB::~DB() = default;

Status DB::Open(const Options&, const std::filesystem::path& path, std::unique_ptr<DB>* out) {
  if (out == nullptr || path.empty()) {
    return Status::InvalidArgument("Open requires a directory and output pointer");
  }
  auto status = platform::CreateDirectories(path);
  if (!status.ok()) {
    return status;
  }
  auto impl = std::make_unique<Impl>();
  status = platform::File::Open(path / "LOCK", true, impl->lock);
  if (!status.ok()) {
    return status;
  }
  status = impl->lock->Lock();
  if (!status.ok()) {
    return status;
  }
  const auto wal_path = path / "WAL";
  std::error_code error;
  const bool exists = std::filesystem::exists(wal_path, error);
  if (error) {
    return Status::IOError("inspect WAL: " + error.message());
  }
  std::unique_ptr<platform::File> file;
  if (!exists) {
    const auto temporary = path / "WAL.tmp";
    status = platform::File::Open(temporary, true, file);
    if (status.ok()) {
      status = file->Truncate(0);
    }
    if (status.ok()) {
      status = file->Sync();
    }
    file.reset();
    if (status.ok()) {
      status = platform::AtomicRename(temporary, wal_path);
    }
    if (!status.ok()) {
      return status;
    }
  }
  status = platform::File::Open(wal_path, false, file);
  if (!status.ok()) {
    return status;
  }
  impl->wal = std::make_unique<internal::Wal>(std::move(file));
  std::uint64_t sequence = 0;
  status = impl->wal->Replay(impl->table, sequence);
  if (!status.ok()) {
    return status;
  }
  impl->sequence.store(sequence, std::memory_order_release);
  *out = std::unique_ptr<DB>(new DB(std::move(impl)));
  return {};
}

Status DB::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch batch;
  const auto status = batch.Put(key, value);
  return status.ok() ? Write(options, &batch) : status;
}

Status DB::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  const auto status = batch.Delete(key);
  return status.ok() ? Write(options, &batch) : status;
}

Status DB::Write(const WriteOptions& options, WriteBatch* batch) {
  if (batch == nullptr) {
    return Status::InvalidArgument("null batch");
  }
  const std::lock_guard guard(impl_->writer);
  if (!impl_->failure.ok()) {
    return impl_->failure;
  }
  if (batch->Count() == 0) {
    return {};
  }
  auto sequence = impl_->sequence.load(std::memory_order_relaxed) + 1;
  auto status = impl_->wal->Append(*batch, sequence, options.sync);
  if (!status.ok()) {
    if (status.code() == StatusCode::kIOError) {
      impl_->failure = status;
    }
    return status;
  }
  std::uint64_t bytes = 0;
  for (const auto& entry : batch->Entries()) {
    status = impl_->table.Insert(sequence++, entry.type, entry.key, entry.value);
    if (!status.ok()) {
      impl_->failure = status;
      return status;
    }
    bytes += entry.key.size() + entry.value.size();
  }
  impl_->user_bytes += bytes;
  // Readers take one committed sequence before traversing. No partial batch
  // becomes visible, even when it updates the same key more than once.
  impl_->sequence.store(sequence - 1, std::memory_order_release);
  return {};
}

Status DB::Get(const ReadOptions& options, std::string_view key, std::string* value) {
  if (value == nullptr || options.snapshot != nullptr) {
    return Status::InvalidArgument("Get requires an output; snapshots are unavailable in M2");
  }
  return impl_->table.Get(key, impl_->sequence.load(std::memory_order_acquire), *value);
}

Stats DB::GetStats() {
  const std::lock_guard guard(impl_->writer);
  Stats stats;
  stats.user_bytes_written = impl_->user_bytes;
  stats.disk_bytes_written = impl_->wal->BytesWritten();
  stats.wal_syncs = impl_->wal->SyncCount();
  stats.recovered_tail_bytes = impl_->wal->TruncatedBytes();
  stats.memtable_bytes = impl_->table.MemoryUsage();
  return stats;
}
}  // namespace lsm
