#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <thread>

#include "lsm/db.hpp"
#include "temp_directory.hpp"
#include "test_support.hpp"
#include "version_set.hpp"

namespace lsm {
namespace {
auto WaitFor(const std::function<bool()>& ready) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

auto Count(DB& db, std::size_t level) -> std::uint64_t {
  const auto counts = db.GetStats().level_file_counts;
  return level < counts.size() ? counts[level] : 0;
}

void CheckView(DB& db, const Snapshot* snapshot,
               const std::map<std::string, std::string>& expected) {
  const ReadOptions options{snapshot};
  for (unsigned key = 0; key < 32; ++key) {
    const auto name = std::to_string(key);
    std::string value = "unchanged";
    const auto status = db.Get(options, name, &value);
    if (expected.contains(name)) {
      ASSERT_TRUE(status.ok()) << status.ToString();
      EXPECT_EQ(value, expected.at(name));
    } else {
      EXPECT_TRUE(status.IsNotFound()) << status.ToString();
      EXPECT_EQ(value, "unchanged");
    }
  }
  auto iterator = db.NewIterator(options);
  ASSERT_TRUE(iterator->status().ok()) << iterator->status().ToString();
  iterator->SeekToFirst();
  for (const auto& [key, value] : expected) {
    ASSERT_TRUE(iterator->Valid());
    EXPECT_EQ(iterator->key(), key);
    EXPECT_EQ(iterator->value(), value);
    iterator->Next();
  }
  EXPECT_FALSE(iterator->Valid());
  iterator->SeekToLast();
  for (auto it = expected.rbegin(); it != expected.rend(); ++it) {
    ASSERT_TRUE(iterator->Valid());
    EXPECT_EQ(iterator->key(), it->first);
    EXPECT_EQ(iterator->value(), it->second);
    iterator->Prev();
  }
  EXPECT_FALSE(iterator->Valid());
  for (unsigned key = 0; key < 40; ++key) {
    const auto target = std::to_string(key);
    iterator->Seek(target);
    auto found = expected.lower_bound(target);
    ASSERT_EQ(iterator->Valid(), found != expected.end());
    const auto limit = target + '\xff';
    for (; found != expected.end() && found->first < limit; ++found) {
      ASSERT_TRUE(iterator->Valid());
      EXPECT_EQ(iterator->key(), found->first);
      EXPECT_EQ(iterator->value(), found->second);
      iterator->Next();
    }
    if (iterator->Valid()) {
      EXPECT_GE(iterator->key(), limit);
    }
  }
  EXPECT_TRUE(iterator->status().ok());
}

TEST(SnapshotTest, RandomBatchesSnapshotsAndIteratorsMatchHistoricalMapsDuringCompaction) {
  test::TempDirectory directory;
  const Options options{.memtable_bytes = 65536, .l0_compaction_trigger = 2};
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  std::mt19937_64 random(test::seed);
  std::map<std::string, std::string> expected;
  std::vector<std::pair<const Snapshot*, std::map<std::string, std::string>>> snapshots;
  for (unsigned operation = 0; operation < 1000; ++operation) {
    SCOPED_TRACE(operation);
    const auto key = std::to_string(random() % 32);
    if (random() % 4 == 0) {
      ASSERT_TRUE(db->Delete({}, key).ok());
      expected.erase(key);
    } else {
      const std::string value(static_cast<std::size_t>(random() % 2048),
                              static_cast<char>(random()));
      WriteBatch batch;
      ASSERT_TRUE(batch.Put(key, "intermediate").ok());
      ASSERT_TRUE(batch.Delete(key).ok());
      ASSERT_TRUE(batch.Put(key, value).ok());
      ASSERT_TRUE(db->Write({}, &batch).ok());
      expected[key] = value;
    }
    if (random() % 47 == 0) snapshots.emplace_back(db->GetSnapshot(), expected);
    if (snapshots.size() > 6) {
      db->ReleaseSnapshot(snapshots.front().first);
      snapshots.erase(snapshots.begin());
    }
    if (operation % 101 == 0) {
      CheckView(*db, nullptr, expected);
      for (const auto& [snapshot, reference] : snapshots) CheckView(*db, snapshot, reference);
    }
  }
  CheckView(*db, nullptr, expected);
  for (const auto& [snapshot, reference] : snapshots) {
    CheckView(*db, snapshot, reference);
    db->ReleaseSnapshot(snapshot);
  }
  db.reset();
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  EXPECT_GT(Count(*db, 1), 0U);
  CheckView(*db, nullptr, expected);
}

TEST(SnapshotTest, VersionsStayAliveUntilSnapshotAndIteratorReleaseThenFilesAreReclaimed) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(
      DB::Open({.memtable_bytes = 1, .l0_compaction_trigger = 2}, directory.path(), &db).ok());
  ASSERT_TRUE(db->Put({true}, "key", "old").ok());
  ASSERT_TRUE(WaitFor([&] { return Count(*db, 0) == 1; }));
  const auto* snapshot = db->GetSnapshot();
  auto iterator = db->NewIterator({snapshot});
  ASSERT_TRUE(iterator->status().ok());
  const auto old_table = internal::TablePath(directory.path(), 4);
  ASSERT_TRUE(std::filesystem::exists(old_table));
  ASSERT_TRUE(db->Put({true}, "key", "new").ok());
  ASSERT_TRUE(WaitFor([&] { return Count(*db, 0) == 0 && Count(*db, 1) > 0; }));
  std::string value;
  ASSERT_TRUE(db->Get({snapshot}, "key", &value).ok());
  EXPECT_EQ(value, "old");
  ASSERT_TRUE(db->Get({}, "key", &value).ok());
  EXPECT_EQ(value, "new");
  EXPECT_TRUE(std::filesystem::exists(old_table));
  db->ReleaseSnapshot(snapshot);
  iterator->SeekToFirst();
  ASSERT_TRUE(iterator->Valid());
  EXPECT_EQ(iterator->value(), "old");
  EXPECT_TRUE(std::filesystem::exists(old_table));
  iterator.reset();
  ASSERT_TRUE(WaitFor([&] {
    // Windows may deny attribute access while the worker's delete is pending.
    // An error is not proof of reclamation: wait for confirmed absence.
    std::error_code error;
    const bool exists = std::filesystem::exists(old_table, error);
    return !error && !exists;
  }));
  db.reset();
  std::unique_ptr<internal::VersionSet> versions;
  ASSERT_TRUE(internal::VersionSet::Open(directory.path(), versions).ok());
  const auto state = versions->State();
  std::set<std::filesystem::path> expected{"CURRENT", "MANIFEST-1", "LOCK"};
  expected.insert(internal::WalPath({}, state.metadata.current_wal));
  for (const auto& file : state.version->files)
    expected.insert(internal::TablePath({}, file.number));
  std::set<std::filesystem::path> actual;
  for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
    actual.insert(entry.path().filename());
  }
  EXPECT_EQ(actual, expected);
}

TEST(SnapshotTest, HigherLevelCompactionAndTombstonesDoNotResurrectDeeperValues) {
  test::TempDirectory directory;
  const Options options{.memtable_bytes = 1, .l0_compaction_trigger = 2};
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  const std::string value(1 << 20, 'x');
  for (unsigned index = 0; index < 12; ++index) {
    const auto key = (index < 10 ? "0" : "") + std::to_string(index);
    ASSERT_TRUE(db->Put({}, key, value).ok());
  }
  ASSERT_TRUE(WaitFor([&] { return Count(*db, 2) > 0 && Count(*db, 0) == 0; }));
  const auto* snapshot = db->GetSnapshot();
  ASSERT_TRUE(db->Delete({true}, "00").ok());
  ASSERT_TRUE(db->Put({}, "extra", "value").ok());
  ASSERT_TRUE(WaitFor([&] { return Count(*db, 0) == 0; }));
  std::string actual;
  ASSERT_TRUE(db->Get({snapshot}, "00", &actual).ok());
  EXPECT_EQ(actual, value);
  EXPECT_TRUE(db->Get({}, "00", &actual).IsNotFound());
  db->ReleaseSnapshot(snapshot);
  db.reset();
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  EXPECT_TRUE(db->Get({}, "00", &actual).IsNotFound());
  ASSERT_TRUE(db->Get({}, "01", &actual).ok());
  EXPECT_EQ(actual, value);
}

TEST(SnapshotTest, ForeignSnapshotsAndInvalidCompactionOptionsAreRejected) {
  test::TempDirectory left;
  test::TempDirectory right;
  std::unique_ptr<DB> first;
  std::unique_ptr<DB> second;
  ASSERT_TRUE(DB::Open({}, left.path(), &first).ok());
  ASSERT_TRUE(DB::Open({}, right.path(), &second).ok());
  const auto* snapshot = first->GetSnapshot();
  std::string output = "unchanged";
  EXPECT_EQ(second->Get({snapshot}, "key", &output).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(output, "unchanged");
  auto iterator = second->NewIterator({snapshot});
  EXPECT_EQ(iterator->status().code(), StatusCode::kInvalidArgument);
  iterator.reset();
  second->ReleaseSnapshot(snapshot);
  first->ReleaseSnapshot(snapshot);
  for (int mode = 0; mode < 5; ++mode) {
    Options options;
    if (mode == 0) options.l0_compaction_trigger = 0;
    if (mode == 1) options.l0_compaction_trigger = -1;
    if (mode == 2) options.level_size_ratio = 1.0;
    if (mode == 3) options.level_size_ratio = std::numeric_limits<double>::infinity();
    if (mode == 4) options.level_size_ratio = std::numeric_limits<double>::quiet_NaN();
    std::unique_ptr<DB> invalid;
    const auto path = left.path() / "invalid";
    EXPECT_EQ(DB::Open(options, path, &invalid).code(), StatusCode::kInvalidArgument);
    EXPECT_FALSE(std::filesystem::exists(path));
  }
}

TEST(SnapshotTest, IteratorCreationReadsNothingAndShortScanVisitsOnlyNeededTables) {
  test::TempDirectory directory;
  const Options options{.memtable_bytes = 65536, .l0_compaction_trigger = 2};
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  const std::string value(512, 'v');
  for (unsigned key = 100000; key < 106000; ++key)
    ASSERT_TRUE(db->Put({}, std::to_string(key), value).ok());
  db.reset();
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  ASSERT_GT(Count(*db, 1), 8U);
  const auto before = internal::table::TableReader::DataBlocksRead();
  auto iterator = db->NewIterator({});
  EXPECT_EQ(internal::table::TableReader::DataBlocksRead(), before);
  iterator->Seek("100000");
  for (unsigned key = 100000; key < 100005; ++key) {
    ASSERT_TRUE(iterator->Valid()) << iterator->status().ToString();
    EXPECT_EQ(iterator->key(), std::to_string(key));
    EXPECT_EQ(iterator->value(), value);
    iterator->Next();
  }
  EXPECT_TRUE(iterator->status().ok());
  EXPECT_LE(internal::table::TableReader::DataBlocksRead() - before, Count(*db, 0) + 2);
}

TEST(SnapshotTest, FailedCompactionPublicationRetainsInputsForRecovery) {
  test::TempDirectory directory;
  const Options options{.memtable_bytes = 1, .l0_compaction_trigger = 2};
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  const auto blocked = internal::TablePath(directory.path(), 7);
  ASSERT_TRUE(std::filesystem::create_directory(blocked));
  ASSERT_TRUE(db->Put({true}, "first", "one").ok());
  ASSERT_TRUE(db->Put({true}, "second", "two").ok());
  // Close drains the pending flush and attempts the now-eligible compaction.
  db.reset();
  EXPECT_TRUE(std::filesystem::exists(internal::TablePath(directory.path(), 4)));
  EXPECT_TRUE(std::filesystem::exists(internal::TablePath(directory.path(), 6)));
  ASSERT_TRUE(std::filesystem::remove(blocked));
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get({}, "first", &value).ok());
  EXPECT_EQ(value, "one");
  ASSERT_TRUE(db->Get({}, "second", &value).ok());
  EXPECT_EQ(value, "two");
  ASSERT_TRUE(WaitFor([&] { return Count(*db, 0) == 0 && Count(*db, 1) > 0; }));
  ASSERT_TRUE(db->Put({true}, "later", "recovered").ok());
}

TEST(SnapshotTest, ConcurrentSnapshotReadersSeeAtomicBatchesDuringCompaction) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(
      DB::Open({.memtable_bytes = 65536, .l0_compaction_trigger = 2}, directory.path(), &db).ok());
  std::atomic<bool> done{false};
  std::atomic<bool> valid{true};
  std::vector<std::thread> readers;
  for (unsigned reader = 0; reader < 4; ++reader) {
    readers.emplace_back([&] {
      while (!done.load()) {
        const auto* snapshot = db->GetSnapshot();
        for (unsigned writer = 0; writer < 2; ++writer) {
          std::string first;
          std::string second;
          const auto prefix = std::to_string(writer);
          const auto a = db->Get({snapshot}, prefix + "a", &first);
          const auto b = db->Get({snapshot}, prefix + "b", &second);
          if (a.code() != b.code() || (a.ok() && first != second) || (!a.ok() && !a.IsNotFound()))
            valid.store(false);
        }
        db->ReleaseSnapshot(snapshot);
      }
    });
  }
  std::vector<std::thread> writers;
  for (unsigned writer = 0; writer < 2; ++writer) {
    writers.emplace_back([&, writer] {
      const auto prefix = std::to_string(writer);
      for (unsigned iteration = 0; iteration < 200; ++iteration) {
        WriteBatch batch;
        const std::string value(1024, static_cast<char>(iteration));
        if (!batch.Put(prefix + "a", value).ok() || !batch.Put(prefix + "b", value).ok() ||
            !db->Write({}, &batch).ok())
          valid.store(false);
      }
    });
  }
  for (auto& writer : writers) writer.join();
  done.store(true);
  for (auto& reader : readers) reader.join();
  EXPECT_TRUE(valid.load());
}
}  // namespace
}  // namespace lsm
