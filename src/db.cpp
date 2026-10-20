#include "lsm/db.hpp"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "file_cleanup.hpp"
#include "memtable.hpp"
#include "platform/file.hpp"
#include "table/format.hpp"
#include "version_set.hpp"
#include "wal.hpp"

namespace lsm {
namespace {
auto ValidateOptions(const Options& options) -> Status {
  if (options.memtable_bytes == 0 || options.block_bytes == 0 ||
      options.block_bytes > std::numeric_limits<std::uint32_t>::max() - 8U ||
      options.bloom_bits_per_key < 0 || options.bloom_bits_per_key > 30) {
    return Status::InvalidArgument("invalid memtable, block size or bloom bits per key");
  }
  return {};
}

auto RemoveUnused(const std::filesystem::path& path) -> Status {
  std::error_code error;
  std::filesystem::remove(path, error);
  // Reappearance after a crash is harmless: only manifest-referenced WALs/tables
  // participate in recovery. Publication uses the stronger sync/rename protocol.
  return error ? Status::IOError("remove unused file: " + error.message()) : Status{};
}

auto OpenWal(const std::filesystem::path& path, std::unique_ptr<internal::Wal>& output) -> Status {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) {
    return Status::IOError("inspect WAL: " + error.message());
  }
  if (!exists) {
    return Status::Corruption("manifest references a missing WAL");
  }
  std::unique_ptr<platform::File> file;
  auto status = platform::File::Open(path, false, file);
  if (status.ok()) {
    output = std::make_unique<internal::Wal>(std::move(file));
  }
  return status;
}
}  // namespace

struct DB::Impl {
  // The directory lock outlives all handles and the joined background worker.
  std::unique_ptr<platform::File> lock;
  std::filesystem::path directory;
  Options options;
  std::unique_ptr<internal::VersionSet> versions;
  std::unique_ptr<internal::Wal> wal;
  std::shared_ptr<internal::MemTable> table = std::make_shared<internal::MemTable>();
  std::shared_ptr<internal::MemTable> immutable;
  std::mutex mutex;
  std::condition_variable changed;
  std::thread worker;
  bool closing = false;
  std::atomic<std::uint64_t> sequence{0};
  std::uint64_t immutable_sequence = 0;
  std::uint64_t user_bytes = 0;
  std::uint64_t retired_wal_bytes = 0;
  std::uint64_t retired_wal_syncs = 0;
  std::uint64_t table_bytes = 0;
  std::uint64_t recovered_tail_bytes = 0;
  Status failure;

  ~Impl() {
    {
      const std::lock_guard guard(mutex);
      closing = true;
      changed.notify_all();
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  auto MakeRoom(std::unique_lock<std::mutex>& guard) -> Status {
    while (table->MemoryUsage() > options.memtable_bytes) {
      changed.wait(guard, [&] { return !immutable || !failure.ok(); });
      if (!failure.ok()) {
        return failure;
      }
      // Another waiting writer may have rotated while this writer was asleep.
      if (table->MemoryUsage() <= options.memtable_bytes) {
        break;
      }
      auto edit = versions->State().metadata;
      if (edit.next_file > std::numeric_limits<std::uint64_t>::max() - 2) {
        failure = Status::IOError("file number exhausted");
        return failure;
      }
      const auto next_wal = edit.next_file++;
      const auto pending_table = edit.next_file++;
      failure = wal->Sync();
      if (!failure.ok()) {
        return failure;
      }
      const auto path = internal::WalPath(directory, next_wal);
      failure = internal::WriteAtomicFile(path, {});
      std::unique_ptr<internal::Wal> next;
      if (failure.ok()) {
        failure = OpenWal(path, next);
      }
      edit.previous_wal = edit.current_wal;
      edit.current_wal = next_wal;
      edit.pending_table = pending_table;
      edit.last_sequence = sequence.load(std::memory_order_acquire);
      if (failure.ok()) {
        failure = versions->Apply(edit);
      }
      if (!failure.ok()) {
        return failure;
      }
      retired_wal_bytes += wal->BytesWritten();
      retired_wal_syncs += wal->SyncCount();
      wal = std::move(next);
      immutable = std::move(table);
      immutable_sequence = edit.last_sequence;
      table = std::make_shared<internal::MemTable>();
      changed.notify_all();
    }
    return failure;
  }

  void FlushLoop() {
    std::unique_lock guard(mutex);
    for (;;) {
      changed.wait(guard, [&] { return closing || (immutable && failure.ok()); });
      if (closing && (!immutable || !failure.ok())) {
        return;
      }
      const auto frozen = immutable;
      const auto last = immutable_sequence;
      const auto number = versions->State().metadata.pending_table;
      const auto previous_wal = versions->State().metadata.previous_wal;
      guard.unlock();
      const auto entries = frozen->Entries();
      const auto path = internal::TablePath(directory, number);
      auto status = internal::table::WriteTable(
          path, entries,
          {.block_bytes = options.block_bytes, .bloom_bits_per_key = options.bloom_bits_per_key});
      std::error_code error;
      std::uint64_t size = 0;
      if (status.ok()) {
        size = std::filesystem::file_size(path, error);
        if (error) {
          status = Status::IOError("flushed table size: " + error.message());
        }
      }
      guard.lock();
      if (status.ok()) {
        auto edit = versions->State().metadata;
        edit.flushed_sequence = last;
        edit.previous_wal = 0;
        edit.pending_table = 0;
        edit.added.push_back(
            {.number = number,
             .level = 0,
             .size = size,
             .smallest = std::string(internal::table::UserKey(entries.front().key)),
             .largest = std::string(internal::table::UserKey(entries.back().key)),
             .reader = {}});
        status = versions->Apply(edit);
        if (status.ok()) {
          table_bytes += size;
          immutable.reset();
          static_cast<void>(internal::RemoveObsolete(internal::WalPath(directory, previous_wal)));
        }
      }
      if (!status.ok()) {
        failure = status;
      }
      changed.notify_all();
    }
  }
};

DB::DB(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DB::~DB() = default;

auto DB::Open(const Options& options, const std::filesystem::path& path, std::unique_ptr<DB>* out)
    -> Status {
  if (out == nullptr || path.empty()) {
    return Status::InvalidArgument("Open requires a directory and output pointer");
  }
  auto status = ValidateOptions(options);
  if (status.ok()) {
    status = platform::CreateDirectories(path);
  }
  if (!status.ok()) {
    return status;
  }
  auto impl = std::make_unique<Impl>();
  impl->directory = path;
  impl->options = options;
  status = platform::File::Open(path / "LOCK", true, impl->lock);
  if (status.ok()) {
    status = impl->lock->Lock();
  }
  if (status.ok()) {
    status = internal::VersionSet::Open(path, impl->versions);
  }
  if (!status.ok()) {
    return status;
  }
  const auto& metadata = impl->versions->State().metadata;
  std::uint64_t sequence = metadata.flushed_sequence;
  if (metadata.previous_wal != 0) {
    std::unique_ptr<internal::Wal> previous;
    status = OpenWal(internal::WalPath(path, metadata.previous_wal), previous);
    impl->immutable = std::make_shared<internal::MemTable>();
    if (status.ok()) {
      status = previous->Replay(*impl->immutable, sequence, sequence + 1);
    }
    if (!status.ok()) {
      return status;
    }
    if (sequence != metadata.last_sequence || impl->immutable->MemoryUsage() == 0) {
      return Status::Corruption("previous WAL disagrees with manifest sequence");
    }
    impl->immutable_sequence = sequence;
    impl->recovered_tail_bytes += previous->TruncatedBytes();
    status = RemoveUnused(internal::TablePath(path, metadata.pending_table));
    if (!status.ok()) {
      return status;
    }
  }
  status = OpenWal(internal::WalPath(path, metadata.current_wal), impl->wal);
  if (status.ok()) {
    status = impl->wal->Replay(*impl->table, sequence, sequence + 1);
  }
  if (!status.ok()) {
    return status;
  }
  impl->recovered_tail_bytes += impl->wal->TruncatedBytes();
  impl->sequence.store(sequence, std::memory_order_release);
  internal::CleanupUnreferenced(path, impl->versions->State());
  try {
    impl->worker = std::thread([state = impl.get()] { state->FlushLoop(); });
  } catch (const std::system_error& error) {
    return Status::IOError("start flush worker: " + std::string(error.what()));
  }
  *out = std::unique_ptr<DB>(new DB(std::move(impl)));
  return {};
}

auto DB::Put(const WriteOptions& options, std::string_view key, std::string_view value) -> Status {
  WriteBatch batch;
  const auto status = batch.Put(key, value);
  return status.ok() ? Write(options, &batch) : status;
}

auto DB::Delete(const WriteOptions& options, std::string_view key) -> Status {
  WriteBatch batch;
  const auto status = batch.Delete(key);
  return status.ok() ? Write(options, &batch) : status;
}

auto DB::Write(const WriteOptions& options, WriteBatch* batch) -> Status {
  if (batch == nullptr) {
    return Status::InvalidArgument("null batch");
  }
  std::unique_lock guard(impl_->mutex);
  if (!impl_->failure.ok()) {
    return impl_->failure;
  }
  if (batch->Count() == 0) {
    return {};
  }
  auto status = impl_->MakeRoom(guard);
  if (!status.ok()) {
    return status;
  }
  auto sequence = impl_->sequence.load(std::memory_order_relaxed) + 1;
  status = impl_->wal->Append(*batch, sequence, options.sync);
  if (!status.ok()) {
    if (status.code() == StatusCode::kIOError) {
      impl_->failure = status;
    }
    return status;
  }
  std::uint64_t bytes = 0;
  for (const auto& entry : batch->Entries()) {
    status = impl_->table->Insert(sequence++, entry.type, entry.key, entry.value);
    if (!status.ok()) {
      impl_->failure = status;
      return status;
    }
    bytes += entry.key.size() + entry.value.size();
  }
  impl_->user_bytes += bytes;
  impl_->sequence.store(sequence - 1, std::memory_order_release);
  // The batch is committed. Rotation failures are sticky for later writes,
  // but cannot change the acknowledgement of this already-applied batch.
  static_cast<void>(impl_->MakeRoom(guard));
  return {};
}

auto DB::Get(const ReadOptions& options, std::string_view key, std::string* value) -> Status {
  if (value == nullptr || options.snapshot != nullptr) {
    return Status::InvalidArgument("Get requires an output; snapshots are unavailable until M5");
  }
  std::shared_ptr<internal::MemTable> active;
  std::shared_ptr<internal::MemTable> immutable;
  std::shared_ptr<const internal::Version> version;
  std::uint64_t sequence = 0;
  {
    const std::lock_guard guard(impl_->mutex);
    active = impl_->table;
    immutable = impl_->immutable;
    version = impl_->versions->State().version;
    sequence = impl_->sequence.load(std::memory_order_acquire);
  }
  internal::table::Entry entry;
  auto status = active->Lookup(key, sequence, entry);
  if (status.IsNotFound() && immutable) {
    status = immutable->Lookup(key, sequence, entry);
  }
  for (std::uint32_t level = 0; status.IsNotFound() && level <= 6; ++level) {
    for (auto it = version->files.rbegin(); status.IsNotFound() && it != version->files.rend();
         ++it) {
      if (it->level == level && key >= it->smallest && key <= it->largest) {
        status = it->reader->Get(key, sequence, entry);
      }
    }
  }
  if (!status.ok()) {
    return status;
  }
  if ((internal::table::Tag(entry.key) & 0xffU) == 0) {
    return Status::NotFound("deleted key");
  }
  *value = std::move(entry.value);
  return {};
}

auto DB::GetStats() -> Stats {
  const std::lock_guard guard(impl_->mutex);
  Stats stats;
  stats.user_bytes_written = impl_->user_bytes;
  stats.disk_bytes_written = impl_->retired_wal_bytes + impl_->wal->BytesWritten() +
                             impl_->table_bytes + impl_->versions->BytesWritten();
  stats.wal_syncs = impl_->retired_wal_syncs + impl_->wal->SyncCount();
  stats.recovered_tail_bytes = impl_->recovered_tail_bytes;
  stats.memtable_bytes =
      impl_->table->MemoryUsage() + (impl_->immutable ? impl_->immutable->MemoryUsage() : 0);
  stats.level_file_counts.resize(1);
  for (const auto& file : impl_->versions->State().version->files) {
    if (stats.level_file_counts.size() <= file.level) {
      stats.level_file_counts.resize(static_cast<std::size_t>(file.level) + 1);
    }
    ++stats.level_file_counts[file.level];
  }
  return stats;
}
}  // namespace lsm
