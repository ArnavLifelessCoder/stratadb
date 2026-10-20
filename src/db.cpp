#include "lsm/db.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "compaction.hpp"
#include "file_cleanup.hpp"
#include "iterator.hpp"
#include "memtable.hpp"
#include "platform/file.hpp"
#include "table/format.hpp"
#include "version_set.hpp"
#include "wal.hpp"

namespace lsm {
namespace {
struct ReadView {
  std::shared_ptr<internal::MemTable> active;
  std::shared_ptr<internal::MemTable> immutable;
  std::shared_ptr<const internal::Version> version;
  std::uint64_t sequence = 0;
};

auto ValidateOptions(const Options& options) -> Status {
  if (options.memtable_bytes == 0 || options.block_bytes == 0 ||
      options.block_bytes > std::numeric_limits<std::uint32_t>::max() - 8U ||
      options.bloom_bits_per_key < 0 || options.bloom_bits_per_key > 30) {
    return Status::InvalidArgument("invalid memtable, block size or bloom bits per key");
  }
  if (options.l0_compaction_trigger < 1 || !std::isfinite(options.level_size_ratio) ||
      options.level_size_ratio <= 1.0) {
    return Status::InvalidArgument("invalid compaction trigger or level size ratio");
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

class Snapshot {
 public:
  explicit Snapshot(ReadView captured) : view(std::move(captured)) {}
  ReadView view;
};

struct DB::Impl {
  // The directory lock outlives all handles and the joined background worker.
  std::unique_ptr<platform::File> lock;
  std::filesystem::path directory;
  Options options;
  std::unique_ptr<internal::VersionSet> versions;
  std::unique_ptr<internal::Wal> wal;
  std::shared_ptr<internal::MemTable> table = std::make_shared<internal::MemTable>();
  std::shared_ptr<internal::MemTable> immutable;
  // Writers and manifest transactions serialize separately from read-view capture.
  std::mutex write_mutex;
  std::mutex manifest_mutex;
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
  std::map<const Snapshot*, std::unique_ptr<Snapshot>> snapshots;
  internal::CompactionPointers compaction_pointers;
  bool cleanup_requested = false;
  struct ObsoleteTable {
    std::filesystem::path path;
    std::weak_ptr<const internal::table::TableReader> reader;
  };
  // Only the background thread touches this queue.
  std::vector<ObsoleteTable> obsolete;

  auto Capture(const ReadOptions& read, ReadView& view) -> Status {
    if (read.snapshot != nullptr) {
      const auto found = snapshots.find(read.snapshot);
      if (found == snapshots.end()) {
        return Status::InvalidArgument("snapshot does not belong to this DB");
      }
      view = found->second->view;
    } else {
      view = {.active = table,
              .immutable = immutable,
              .version = versions->State().version,
              .sequence = sequence.load(std::memory_order_acquire)};
    }
    return {};
  }

  auto OldestSnapshot() const -> std::uint64_t {
    auto oldest = sequence.load(std::memory_order_acquire);
    for (const auto& [pointer, snapshot] : snapshots) {
      static_cast<void>(pointer);
      oldest = std::min(oldest, snapshot->view.sequence);
    }
    return oldest;
  }

  void RequestCleanup() {
    const std::lock_guard guard(mutex);
    cleanup_requested = true;
    changed.notify_all();
  }

  void CollectObsolete() {
    std::erase_if(obsolete, [](const auto& file) {
      return file.reader.expired() && internal::RemoveObsolete(file.path);
    });
  }

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
      guard.unlock();
      const std::lock_guard manifest_guard(manifest_mutex);
      guard.lock();
      if (!failure.ok()) {
        return failure;
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
      guard.unlock();
      auto status = internal::WriteAtomicFile(path, {});
      std::unique_ptr<internal::Wal> next;
      if (status.ok()) {
        status = OpenWal(path, next);
      }
      edit.previous_wal = edit.current_wal;
      edit.current_wal = next_wal;
      edit.pending_table = pending_table;
      edit.last_sequence = sequence.load(std::memory_order_acquire);
      if (status.ok()) {
        status = versions->Apply(edit);
      }
      guard.lock();
      if (!status.ok()) {
        failure = status;
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

  struct CompactionResult {
    Status status;
    std::uint64_t bytes = 0;
  };

  auto Compact(const internal::CompactionPlan& plan, std::uint64_t oldest) -> CompactionResult {
    // Reserve/output/commit is one manifest transaction. Readers never take
    // this mutex; writers only wait for it when they need to rotate their WAL.
    const std::lock_guard manifest_guard(manifest_mutex);
    std::vector<std::vector<internal::table::Entry>> streams;
    for (const auto& file : plan.inputs) {
      std::vector<internal::table::Entry> entries;
      const auto status = file.reader->ReadAll(entries);
      if (!status.ok()) {
        return {.status = status};
      }
      streams.push_back(std::move(entries));
    }
    internal::MergingIterator input(std::move(streams));
    std::vector<internal::table::Entry> entries;
    auto status = internal::CompactEntries(input, plan, oldest, entries);
    if (!status.ok()) {
      return {.status = status};
    }
    auto edit = versions->State().metadata;
    for (const auto& file : plan.inputs) {
      edit.removed.push_back(file.number);
    }
    std::uint64_t bytes = 0;
    constexpr std::size_t target_bytes = 2 << 20;
    for (std::size_t begin = 0; begin < entries.size();) {
      auto end = begin;
      std::size_t estimate = 0;
      while (end < entries.size()) {
        if (end != begin && estimate >= target_bytes &&
            internal::table::UserKey(entries[end - 1].key) !=
                internal::table::UserKey(entries[end].key)) {
          break;
        }
        estimate += entries[end].key.size() + entries[end].value.size() + 12;
        ++end;
      }
      if (edit.next_file == std::numeric_limits<std::uint64_t>::max()) {
        return {.status = Status::IOError("compaction file number exhausted")};
      }
      const auto number = edit.next_file++;
      const auto path = internal::TablePath(directory, number);
      status = internal::table::WriteTable(
          path, std::span<const internal::table::Entry>(entries).subspan(begin, end - begin),
          {.block_bytes = options.block_bytes, .bloom_bits_per_key = options.bloom_bits_per_key});
      if (!status.ok()) {
        return {.status = status};
      }
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (error) {
        return {.status = Status::IOError("compacted table size: " + error.message())};
      }
      bytes += size;
      edit.added.push_back({.number = number,
                            .level = plan.level + 1,
                            .size = size,
                            .smallest = std::string(internal::table::UserKey(entries[begin].key)),
                            .largest = std::string(internal::table::UserKey(entries[end - 1].key)),
                            .reader = {}});
      begin = end;
    }
    status = versions->Apply(edit);
    if (status.ok()) {
      for (const auto& file : plan.inputs) {
        obsolete.push_back(
            {.path = internal::TablePath(directory, file.number), .reader = file.reader});
      }
    }
    return {.status = status, .bytes = bytes};
  }

  void FlushLoop() {
    std::unique_lock guard(mutex);
    for (;;) {
      changed.wait_for(guard, std::chrono::milliseconds(50), [&] {
        return closing || cleanup_requested ||
               (failure.ok() &&
                (immutable || internal::PickCompaction(versions->State().version, options,
                                                       compaction_pointers)));
      });
      cleanup_requested = false;
      if (failure.ok() && immutable) {
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
        if (status.ok()) {
          const std::lock_guard manifest_guard(manifest_mutex);
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
        }
        if (status.ok()) {
          static_cast<void>(internal::RemoveObsolete(internal::WalPath(directory, previous_wal)));
        }
        guard.lock();
        if (status.ok()) {
          table_bytes += size;
          immutable.reset();
        } else {
          failure = status;
        }
      } else if (failure.ok()) {
        auto plan =
            internal::PickCompaction(versions->State().version, options, compaction_pointers);
        if (plan) {
          const auto oldest = OldestSnapshot();
          guard.unlock();
          const auto result = Compact(*plan, oldest);
          guard.lock();
          if (result.status.ok()) {
            table_bytes += result.bytes;
            compaction_pointers[plan->level] = plan->largest;
          } else {
            failure = result.status;
          }
        }
      }
      changed.notify_all();
      guard.unlock();
      CollectObsolete();
      guard.lock();
      if (closing && (!failure.ok() ||
                      (!immutable && !internal::PickCompaction(versions->State().version, options,
                                                               compaction_pointers)))) {
        return;
      }
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
  const auto metadata = impl->versions->State().metadata;
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
  const std::lock_guard writer(impl_->write_mutex);
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
  if (value == nullptr) {
    return Status::InvalidArgument("Get requires an output");
  }
  ReadView view;
  {
    const std::lock_guard guard(impl_->mutex);
    const auto status = impl_->Capture(options, view);
    if (!status.ok()) {
      return status;
    }
  }
  internal::table::Entry entry;
  auto status = view.active->Lookup(key, view.sequence, entry);
  if (status.IsNotFound() && view.immutable) {
    status = view.immutable->Lookup(key, view.sequence, entry);
  }
  const auto& files = view.version->files;
  const auto& l0 = view.version->levels[0];
  for (auto it = l0.rbegin(); status.IsNotFound() && it != l0.rend(); ++it) {
    const auto& file = files[*it];
    if (key >= file.smallest && key <= file.largest) {
      status = file.reader->Get(key, view.sequence, entry);
    }
  }
  for (std::size_t level = 1; status.IsNotFound() && level < view.version->levels.size(); ++level) {
    const auto& indices = view.version->levels[level];
    auto found = std::ranges::upper_bound(
        indices, key, {}, [&](auto index) -> const std::string& { return files[index].smallest; });
    if (found == indices.begin()) {
      continue;
    }
    const auto& file = files[*--found];
    if (key <= file.largest) {
      status = file.reader->Get(key, view.sequence, entry);
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

auto DB::GetSnapshot() -> const Snapshot* {
  const std::lock_guard guard(impl_->mutex);
  ReadView view;
  static_cast<void>(impl_->Capture({}, view));
  auto snapshot = std::make_unique<Snapshot>(std::move(view));
  const auto* result = snapshot.get();
  impl_->snapshots.emplace(result, std::move(snapshot));
  return result;
}

void DB::ReleaseSnapshot(const Snapshot* snapshot) {
  std::unique_ptr<Snapshot> released;
  {
    const std::lock_guard guard(impl_->mutex);
    const auto found = impl_->snapshots.find(snapshot);
    if (found == impl_->snapshots.end()) {
      return;
    }
    released = std::move(found->second);
    impl_->snapshots.erase(found);
  }
  released.reset();
  impl_->RequestCleanup();
}

auto DB::NewIterator(const ReadOptions& options) -> std::unique_ptr<Iterator> {
  ReadView view;
  Status status;
  std::vector<std::vector<internal::table::Entry>> streams;
  {
    const std::lock_guard guard(impl_->mutex);
    status = impl_->Capture(options, view);
    if (status.ok()) {
      // The DB mutex freezes insertion while copying the mutable skiplist.
      streams.push_back(view.active->Entries());
      if (view.immutable) {
        streams.push_back(view.immutable->Entries());
      }
    }
  }
  if (status.ok()) {
    for (const auto& file : view.version->files) {
      std::vector<internal::table::Entry> entries;
      status = file.reader->ReadAll(entries);
      if (!status.ok()) {
        break;
      }
      streams.push_back(std::move(entries));
    }
  }
  return internal::MakeIterator(std::move(streams), view.sequence, std::move(view.version),
                                std::move(status),
                                [state = impl_.get()] { state->RequestCleanup(); });
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
  const auto version = impl_->versions->State().version;
  for (const auto& file : version->files) {
    if (stats.level_file_counts.size() <= file.level) {
      stats.level_file_counts.resize(static_cast<std::size_t>(file.level) + 1);
    }
    ++stats.level_file_counts[file.level];
  }
  return stats;
}
}  // namespace lsm
