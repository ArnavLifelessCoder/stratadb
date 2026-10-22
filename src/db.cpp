#include "lsm/db.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "compaction.hpp"
#include "file_cleanup.hpp"
#include "iterator.hpp"
#include "memtable.hpp"
#include "platform/file.hpp"
#include "table/block_cache.hpp"
#include "table/format.hpp"
#include "table/table_iterator.hpp"
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
  if (options.l0_stop_writes < 1) {
    return Status::InvalidArgument("l0_stop_writes must be positive");
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
  // Group commit. Writers queue under write_mutex; the front writer leads,
  // commits the queued batches it can take as one WAL record and wakes them.
  struct Writer {
    Writer(WriteBatch* writer_batch, bool writer_sync) : batch(writer_batch), sync(writer_sync) {}
    WriteBatch* batch;
    bool sync;
    bool done = false;
    Status status;
    std::condition_variable ready;
  };
  std::mutex write_mutex;
  std::deque<Writer*> writers;
  std::shared_ptr<internal::table::BlockCache> cache;
  // Manifest transactions serialize separately from read-view capture.
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
  std::uint64_t write_calls = 0;
  std::uint64_t wal_records = 0;
  std::uint64_t stall_micros = 0;
  std::uint64_t slowed_writes = 0;
  std::uint64_t stopped_writes = 0;
  Status failure;
  std::map<const Snapshot*, std::unique_ptr<Snapshot>> snapshots;
  internal::CompactionPointers compaction_pointers;
  bool cleanup_requested = false;
  struct ObsoleteTable {
    std::filesystem::path path;
    std::weak_ptr<const internal::table::TableReader> reader;
  };
  // Only the background thread touches this queue. obsolete_pending mirrors
  // whether it is non-empty, so releasing a view only wakes that thread when
  // a release may actually let it delete something.
  std::vector<ObsoleteTable> obsolete;
  std::atomic<bool> obsolete_pending{false};

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

  [[nodiscard]] auto OldestSnapshot() const -> std::uint64_t {
    auto oldest = sequence.load(std::memory_order_acquire);
    for (const auto& [pointer, snapshot] : snapshots) {
      static_cast<void>(pointer);
      oldest = std::min(oldest, snapshot->view.sequence);
    }
    return oldest;
  }

  void RequestCleanup() {
    if (!obsolete_pending.load(std::memory_order_acquire)) {
      return;
    }
    const std::lock_guard guard(mutex);
    cleanup_requested = true;
    changed.notify_all();
  }

  void CollectObsolete() {
    std::erase_if(obsolete, [](const auto& file) {
      return file.reader.expired() && internal::RemoveObsolete(file.path);
    });
    obsolete_pending.store(!obsolete.empty(), std::memory_order_release);
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

  static auto MicrosSince(std::chrono::steady_clock::time_point began) -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - began)
                                          .count());
  }

  [[nodiscard]] auto L0Files() const -> std::size_t {
    return versions->State().version->levels[0].size();
  }

  // Leader only, with `guard` held. Delays the write by 1 ms once L0 reaches
  // kSlowdownL0 tables and blocks it while L0 is at the stop threshold, so
  // compaction can catch up. Both count as stall time. The stop threshold is
  // kept above the compaction trigger: stopping below it would wait forever.
  auto Throttle(std::unique_lock<std::mutex>& guard) -> Status {
    static constexpr std::size_t kSlowdownL0 = 8;
    const auto stop = std::max(static_cast<std::size_t>(options.l0_stop_writes),
                               static_cast<std::size_t>(options.l0_compaction_trigger) + 1);
    bool slowed = false;
    for (;;) {
      if (!failure.ok()) {
        return failure;
      }
      const auto files = L0Files();
      if (files >= stop) {
        const auto began = std::chrono::steady_clock::now();
        ++stopped_writes;
        changed.wait(guard, [&] { return !failure.ok() || L0Files() < stop; });
        stall_micros += MicrosSince(began);
        continue;
      }
      if (!slowed && files >= kSlowdownL0) {
        slowed = true;
        ++slowed_writes;
        const auto began = std::chrono::steady_clock::now();
        guard.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        guard.lock();
        stall_micros += MicrosSince(began);
        continue;
      }
      return {};
    }
  }

  // Leader only. Appends the group as one WAL record (one sync when `sync`),
  // applies it to the memtable and publishes its last sequence.
  auto Commit(const std::vector<Writer*>& group, bool sync) -> Status {
    std::unique_lock guard(mutex);
    auto status = Throttle(guard);
    if (status.ok()) {
      status = MakeRoom(guard);
    }
    if (!status.ok()) {
      return status;
    }
    const WriteBatch* record = group.front()->batch;
    WriteBatch combined;
    if (group.size() > 1) {
      for (const auto* writer : group) {
        for (const auto& entry : writer->batch->Entries()) {
          status = entry.type == WriteBatch::Type::kPut ? combined.Put(entry.key, entry.value)
                                                        : combined.Delete(entry.key);
          if (!status.ok()) {
            return status;
          }
        }
      }
      record = &combined;
    }
    auto next = sequence.load(std::memory_order_relaxed) + 1;
    // Only the leader appends or rotates the WAL, so it can drop the read-view
    // mutex for the append and sync.
    guard.unlock();
    status = wal->Append(*record, next, sync);
    guard.lock();
    if (!status.ok()) {
      if (status.code() == StatusCode::kIOError) {
        failure = status;
      }
      return status;
    }
    std::uint64_t bytes = 0;
    for (const auto& entry : record->Entries()) {
      status = table->Insert(next++, entry.type, entry.key, entry.value);
      if (!status.ok()) {
        failure = status;
        return status;
      }
      bytes += entry.key.size() + entry.value.size();
    }
    user_bytes += bytes;
    write_calls += group.size();
    ++wal_records;
    sequence.store(next - 1, std::memory_order_release);
    // The group is committed. Rotation failures are sticky for later writes,
    // but cannot change the acknowledgement of these already-applied batches.
    static_cast<void>(MakeRoom(guard));
    return {};
  }

  auto MakeRoom(std::unique_lock<std::mutex>& guard) -> Status {
    while (table->MemoryUsage() > options.memtable_bytes) {
      if (immutable) {
        const auto began = std::chrono::steady_clock::now();
        changed.wait(guard, [&] { return !immutable || !failure.ok(); });
        stall_micros += MicrosSince(began);
      }
      if (!failure.ok()) {
        return failure;
      }
      // Another waiting writer may have rotated while this writer was asleep.
      if (table->MemoryUsage() <= options.memtable_bytes) {
        break;
      }
      guard.unlock();
      const auto synced = wal->Sync();
      if (!synced.ok()) {
        guard.lock();
        failure = synced;
        return failure;
      }
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
    std::vector<std::unique_ptr<internal::InternalIterator>> children;
    children.reserve(plan.inputs.size());
    for (const auto& file : plan.inputs)
      children.push_back(std::make_unique<internal::table::TableIterator>(file.reader));
    internal::MergingIterator input(std::move(children));
    std::vector<internal::table::Entry> pending;
    std::vector<internal::TableMetadata> outputs;
    std::size_t estimate = 0;
    std::uint64_t bytes = 0;
    constexpr std::size_t target_bytes = 2 << 20;
    const auto flush = [&]() -> Status {
      if (pending.empty()) return {};
      std::uint64_t number = 0;
      {
        // Persist a reservation before releasing the allocator lock. A crash
        // leaves a harmless gap or orphan; a concurrent WAL rotation cannot
        // reuse the output number. No table reads/writes hold this mutex.
        const std::lock_guard manifest_guard(manifest_mutex);
        auto reservation = versions->State().metadata;
        if (reservation.next_file == std::numeric_limits<std::uint64_t>::max())
          return Status::IOError("compaction file number exhausted");
        number = reservation.next_file++;
        const auto status = versions->Apply(reservation);
        if (!status.ok()) return status;
      }
      const auto path = internal::TablePath(directory, number);
      auto status = internal::table::WriteTable(
          path, pending,
          {.block_bytes = options.block_bytes, .bloom_bits_per_key = options.bloom_bits_per_key});
      if (!status.ok()) return status;
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (error) return Status::IOError("compacted table size: " + error.message());
      outputs.push_back({.number = number,
                         .level = plan.level + 1,
                         .size = size,
                         .smallest = std::string(internal::table::UserKey(pending.front().key)),
                         .largest = std::string(internal::table::UserKey(pending.back().key)),
                         .reader = {}});
      bytes += size;
      pending.clear();
      estimate = 0;
      return {};
    };
    std::size_t since_check = 0;
    auto status = internal::CompactEntries(
        input, plan, oldest, [&](const internal::table::Entry& entry) -> Status {
          // A long compaction must not hold up the memtable flush that writers
          // are waiting for. The new L0 table is newer than every input, so it
          // can be published while this merge runs.
          if (++since_check == 1024) {
            since_check = 0;
            std::unique_lock guard(mutex);
            if (failure.ok() && immutable) {
              FlushImmutable(guard);
              changed.notify_all();
            }
            if (!failure.ok()) return failure;
          }
          if (!pending.empty() && estimate >= target_bytes &&
              internal::table::UserKey(pending.back().key) != internal::table::UserKey(entry.key)) {
            const auto result = flush();
            if (!result.ok()) return result;
          }
          estimate += entry.key.size() + entry.value.size() + 12;
          pending.push_back(entry);
          return {};
        });
    if (status.ok()) status = flush();
    if (status.ok()) {
      const std::lock_guard manifest_guard(manifest_mutex);
      // Refresh metadata: writers may have rotated a WAL during the merge.
      auto edit = versions->State().metadata;
      edit.added = std::move(outputs);
      for (const auto& file : plan.inputs) edit.removed.push_back(file.number);
      status = versions->Apply(edit);
    }
    if (status.ok()) {
      for (const auto& file : plan.inputs)
        obsolete.push_back(
            {.path = internal::TablePath(directory, file.number), .reader = file.reader});
      // A release racing with this store is still collected: the background
      // loop calls CollectObsolete at least every 50 ms.
      obsolete_pending.store(true, std::memory_order_release);
    }
    return {.status = status, .bytes = bytes};
  }

  // Background thread only. Writes the immutable memtable to a new L0 table
  // and publishes it. Called with `guard` held and an immutable memtable
  // present; returns with `guard` held. Failures become the sticky DB failure.
  void FlushImmutable(std::unique_lock<std::mutex>& guard) {
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
      edit.added.push_back({.number = number,
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
  }

  [[nodiscard]] auto PlanCompaction() const -> std::optional<internal::CompactionPlan> {
    if (internal::pause_compactions_for_testing.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    return internal::PickCompaction(versions->State().version, options, compaction_pointers);
  }

  void FlushLoop() {
    std::unique_lock guard(mutex);
    for (;;) {
      changed.wait_for(guard, std::chrono::milliseconds(50), [&] {
        return closing || cleanup_requested ||
               (failure.ok() && (immutable || PlanCompaction().has_value()));
      });
      cleanup_requested = false;
      if (failure.ok() && immutable) {
        FlushImmutable(guard);
      } else if (failure.ok()) {
        auto plan = PlanCompaction();
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
      if (closing && (!failure.ok() || (!immutable && !PlanCompaction().has_value()))) {
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
    if (options.block_cache_bytes > 0) {
      impl->cache = std::make_shared<internal::table::BlockCache>(options.block_cache_bytes);
    }
    status = internal::VersionSet::Open(path, impl->versions, impl->cache);
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
  if (batch->Count() == 0) {
    const std::lock_guard guard(impl_->mutex);
    return impl_->failure;
  }
  Impl::Writer self(batch, options.sync);
  std::unique_lock queue(impl_->write_mutex);
  impl_->writers.push_back(&self);
  self.ready.wait(queue, [&] { return self.done || impl_->writers.front() == &self; });
  if (self.done) {
    return self.status;
  }
  // This writer leads. Take the queued writers behind it, up to a size cap so
  // one group does not delay its leader too much. A non-sync leader stops at
  // the first sync writer, which must not be acknowledged without a sync.
  const auto batch_bytes = [](const WriteBatch& b) {
    std::size_t bytes = 0;
    for (const auto& entry : b.Entries()) bytes += entry.key.size() + entry.value.size();
    return bytes;
  };
  const auto leader_bytes = batch_bytes(*batch);
  const std::size_t cap = leader_bytes < (128 << 10) ? leader_bytes + (128 << 10) : 1 << 20;
  std::vector<Impl::Writer*> group;
  std::size_t group_bytes = 0;
  for (auto* writer : impl_->writers) {
    if (!group.empty()) {
      if ((writer->sync && !self.sync) || group_bytes >= cap) {
        break;
      }
    }
    group.push_back(writer);
    group_bytes += batch_bytes(*writer->batch);
  }
  queue.unlock();
  const auto status = impl_->Commit(group, self.sync);
  queue.lock();
  for (auto* writer : group) {
    impl_->writers.pop_front();
    if (writer != &self) {
      writer->status = status;
      writer->done = true;
      writer->ready.notify_one();
    }
  }
  if (!impl_->writers.empty()) {
    impl_->writers.front()->ready.notify_one();
  }
  return status;
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
  std::vector<std::unique_ptr<internal::InternalIterator>> children;
  {
    const std::lock_guard guard(impl_->mutex);
    status = impl_->Capture(options, view);
  }
  if (status.ok()) {
    children = internal::MakeVersionIterators(view.version);
    children.push_back(internal::MakeMemTableIterator(std::move(view.active)));
    if (view.immutable)
      children.push_back(internal::MakeMemTableIterator(std::move(view.immutable)));
  }
  return internal::MakeIterator(std::move(children), view.sequence, std::move(view.version),
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
  stats.write_calls = impl_->write_calls;
  stats.wal_records = impl_->wal_records;
  stats.stall_micros = impl_->stall_micros;
  stats.slowed_writes = impl_->slowed_writes;
  stats.stopped_writes = impl_->stopped_writes;
  if (impl_->cache) {
    const auto counters = impl_->cache->GetCounters();
    stats.block_cache_hits = counters.hits;
    stats.block_cache_misses = counters.misses;
    stats.block_cache_bytes = counters.usage;
  }
  return stats;
}
}  // namespace lsm
