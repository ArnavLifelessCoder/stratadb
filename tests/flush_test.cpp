#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <map>
#include <random>
#include <thread>

#include "lsm/db.hpp"
#include "table/format.hpp"
#include "temp_directory.hpp"
#include "test_support.hpp"
#include "version_set.hpp"
#include "wal.hpp"

namespace lsm {
namespace {
void Check(DB& db, const std::map<std::string, std::string>& expected) {
  for (unsigned i = 0; i < 32; ++i) {
    const auto key = std::to_string(i);
    std::string value = "unchanged";
    const auto status = db.Get({}, key, &value);
    if (expected.contains(key)) {
      ASSERT_TRUE(status.ok()) << status.ToString();
      EXPECT_EQ(value, expected.at(key));
    } else {
      EXPECT_TRUE(status.IsNotFound()) << status.ToString();
      EXPECT_EQ(value, "unchanged");
    }
  }
}

TEST(FlushTest, RejectsActiveOptionLimitsBeforeCreatingFiles) {
  test::TempDirectory directory;
  const auto path = directory.path() / "invalid";
  for (int mode = 0; mode < 5; ++mode) {
    Options options;
    if (mode == 0) options.memtable_bytes = 0;
    if (mode == 1) options.block_bytes = 0;
    if (mode == 2) options.block_bytes = std::numeric_limits<std::size_t>::max();
    if (mode == 3) options.bloom_bits_per_key = -1;
    if (mode == 4) options.bloom_bits_per_key = 31;
    std::unique_ptr<DB> db;
    EXPECT_EQ(DB::Open(options, path, &db).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(db, nullptr);
    EXPECT_FALSE(std::filesystem::exists(path));
  }
}

TEST(FlushTest, ReopenAfterZeroOneAndManyFlushesMatchesMap) {
  for (const unsigned writes : {0U, 1U, 2U, 9U, 32U}) {
    SCOPED_TRACE(writes);
    test::TempDirectory directory;
    const Options options{.memtable_bytes = 1, .block_bytes = 128};
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
    std::mt19937_64 random(test::seed);
    std::map<std::string, std::string> expected;
    for (unsigned write = 0; write < writes; ++write) {
      WriteBatch batch;
      const auto key = std::to_string(random() % 32);
      const auto other = std::to_string(random() % 32);
      const auto value = std::string(write % 17, static_cast<char>(write));
      ASSERT_TRUE(batch.Put(key, "intermediate").ok());
      ASSERT_TRUE(batch.Put(key, value).ok());
      ASSERT_TRUE(batch.Delete(other).ok());
      ASSERT_TRUE(db->Write({write % 2 == 0}, &batch).ok());
      expected[key] = value;
      expected.erase(other);
      Check(*db, expected);
    }
    db.reset();
    ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
    Check(*db, expected);
    EXPECT_EQ(db->GetStats().level_file_counts.at(0), writes);
    db.reset();
    unsigned logs = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
      logs += entry.path().extension() == ".wal";
    }
    EXPECT_EQ(logs, 1U);
  }
}

TEST(FlushTest, LargeValuesAndTombstonesAcrossOverlappingTables) {
  test::TempDirectory directory;
  const Options options{.memtable_bytes = 1};
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  const std::string large(1 << 20, '\xff');
  ASSERT_TRUE(db->Put({true}, "", large).ok());
  ASSERT_TRUE(db->Put({true}, "other", "").ok());
  ASSERT_TRUE(db->Delete({true}, "").ok());
  std::string value = "unchanged";
  EXPECT_TRUE(db->Get({}, "", &value).IsNotFound());
  EXPECT_EQ(value, "unchanged");
  db.reset();
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  EXPECT_TRUE(db->Get({}, "", &value).IsNotFound());
  ASSERT_TRUE(db->Get({}, "other", &value).ok());
  EXPECT_TRUE(value.empty());
  EXPECT_EQ(db->GetStats().level_file_counts.at(0), 3U);
}

TEST(FlushTest, MixedTablesAndCurrentWalMatchMapAcrossRepeatedReopens) {
  SCOPED_TRACE(test::seed);
  test::TempDirectory directory;
  const Options options{.memtable_bytes = 65536, .block_bytes = 512};
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  std::mt19937_64 random(test::seed);
  std::map<std::string, std::string> expected;
  for (unsigned operation = 0; operation < 2000; ++operation) {
    SCOPED_TRACE(operation);
    const auto key = std::to_string(random() % 32);
    if (random() % 4 == 0) {
      ASSERT_TRUE(db->Delete({}, key).ok());
      expected.erase(key);
    } else {
      const auto value =
          std::string(static_cast<std::size_t>(random() % 4096), static_cast<char>(random()));
      WriteBatch batch;
      ASSERT_TRUE(batch.Put(key, "intermediate").ok());
      ASSERT_TRUE(batch.Delete(key).ok());
      ASSERT_TRUE(batch.Put(key, value).ok());
      ASSERT_TRUE(db->Write({operation % 113 == 0}, &batch).ok());
      expected[key] = value;
    }
    if (operation % 137 == 0) {
      db.reset();
      ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
      Check(*db, expected);
    }
  }
  db.reset();
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  Check(*db, expected);
  EXPECT_GT(db->GetStats().level_file_counts.at(0), 10U);
}

TEST(FlushTest, MigratesTheLegacySingleWalAndContinuesItsSequence) {
  test::TempDirectory directory;
  ASSERT_TRUE(internal::WriteAtomicFile(directory.path() / "WAL", {}).ok());
  {
    std::unique_ptr<platform::File> file;
    ASSERT_TRUE(platform::File::Open(directory.path() / "WAL", false, file).ok());
    internal::Wal wal(std::move(file));
    WriteBatch batch;
    ASSERT_TRUE(batch.Put("legacy", "old").ok());
    ASSERT_TRUE(wal.Append(batch, 1, true).ok());
  }
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get({}, "legacy", &value).ok());
  EXPECT_EQ(value, "old");
  ASSERT_TRUE(db->Put({true}, "legacy", "new").ok());
  db.reset();
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  ASSERT_TRUE(db->Get({}, "legacy", &value).ok());
  EXPECT_EQ(value, "new");
}

TEST(FlushTest, RecoversRotationAndUncommittedTablePublication) {
  for (const bool published_table : {false, true}) {
    test::TempDirectory directory;
    std::unique_ptr<internal::VersionSet> versions;
    ASSERT_TRUE(internal::VersionSet::Open(directory.path(), versions).ok());
    std::unique_ptr<platform::File> file;
    ASSERT_TRUE(platform::File::Open(internal::WalPath(directory.path(), 2), false, file).ok());
    internal::Wal wal(std::move(file));
    WriteBatch batch;
    ASSERT_TRUE(batch.Put("key", "durable").ok());
    ASSERT_TRUE(wal.Append(batch, 1, true).ok());
    ASSERT_TRUE(internal::WriteAtomicFile(internal::WalPath(directory.path(), 3), {}).ok());
    auto edit = versions->State().metadata;
    edit.next_file = 5;
    edit.last_sequence = 1;
    edit.current_wal = 3;
    edit.previous_wal = 2;
    edit.pending_table = 4;
    ASSERT_TRUE(versions->Apply(edit).ok());
    if (published_table) {
      const std::vector<internal::table::Entry> orphan{
          {.key = internal::table::InternalKey("key", 1, WriteBatch::Type::kPut),
           .value = "durable"}};
      ASSERT_TRUE(
          internal::table::WriteTable(internal::TablePath(directory.path(), 4), orphan).ok());
    }
    versions.reset();
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open({.memtable_bytes = 1}, directory.path(), &db).ok());
    std::string value;
    ASSERT_TRUE(db->Get({}, "key", &value).ok());
    EXPECT_EQ(value, "durable");
    ASSERT_TRUE(db->Put({true}, "later", "second").ok());
    db.reset();
    ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
    ASSERT_TRUE(db->Get({}, "key", &value).ok());
    EXPECT_EQ(value, "durable");
    ASSERT_TRUE(db->Get({}, "later", &value).ok());
    EXPECT_EQ(value, "second");
  }
}

TEST(FlushTest, BackgroundFailureStopsWritesAndRecoveryRetainsBothLogs) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  const Options options{.memtable_bytes = 1};
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  ASSERT_TRUE(std::filesystem::create_directory(internal::TablePath(directory.path(), 4)));
  ASSERT_TRUE(db->Put({true}, "first", "one").ok());
  const auto second = db->Put({true}, "second", "two");
  if (second.ok()) {
    std::string applied;
    ASSERT_TRUE(db->Get({}, "second", &applied).ok());
    EXPECT_EQ(applied, "two");
  }
  EXPECT_FALSE(db->Put({true}, "third", "three").ok());
  db.reset();
  ASSERT_TRUE(std::filesystem::remove(internal::TablePath(directory.path(), 4)));
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get({}, "first", &value).ok());
  EXPECT_EQ(value, "one");
  // A failed write may have appended before the background error became visible.
  const auto status = db->Get({}, "second", &value);
  EXPECT_TRUE(status.ok() || status.IsNotFound());
  if (status.ok()) {
    EXPECT_EQ(value, "two");
  }
  ASSERT_TRUE(db->Put({true}, "after", "recovered").ok());
}

TEST(FlushTest, RotationFailureDoesNotRejectAnAlreadyAppliedBatch) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({.memtable_bytes = 1}, directory.path(), &db).ok());
  const auto blocked = internal::WalPath(directory.path(), 3);
  ASSERT_TRUE(std::filesystem::create_directory(blocked));
  ASSERT_TRUE(db->Put({true}, "accepted", "value").ok());
  std::string value;
  ASSERT_TRUE(db->Get({}, "accepted", &value).ok());
  EXPECT_EQ(value, "value");
  EXPECT_FALSE(db->Put({true}, "rejected", "value").ok());
  EXPECT_TRUE(db->Get({}, "rejected", &value).IsNotFound());
  db.reset();
  ASSERT_TRUE(std::filesystem::remove(blocked));
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  ASSERT_TRUE(db->Get({}, "accepted", &value).ok());
  EXPECT_EQ(value, "value");
  EXPECT_TRUE(db->Get({}, "rejected", &value).IsNotFound());
}

TEST(FlushTest, MissingReferencedWalOrTableIsCorruption) {
  for (const bool remove_table : {false, true}) {
    test::TempDirectory directory;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open({.memtable_bytes = 1}, directory.path(), &db).ok());
    ASSERT_TRUE(db->Put({true}, "key", "value").ok());
    db.reset();
    const auto path = remove_table ? internal::TablePath(directory.path(), 4)
                                   : internal::WalPath(directory.path(), 3);
    ASSERT_TRUE(std::filesystem::remove(path));
    EXPECT_EQ(DB::Open({}, directory.path(), &db).code(), StatusCode::kCorruption);
    EXPECT_EQ(db, nullptr);
  }
}

TEST(FlushTest, ConcurrentReadersAndWritersAcrossBackgroundFlushes) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({.memtable_bytes = 65536}, directory.path(), &db).ok());
  ASSERT_TRUE(db->Put({}, "shared", "committed").ok());
  std::atomic<bool> done{false};
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!done.load()) {
        std::string value;
        if (!db->Get({}, "shared", &value).ok() || value != "committed") ++failures;
      }
    });
  }
  std::vector<std::thread> writers;
  for (unsigned writer = 0; writer < 2; ++writer) {
    writers.emplace_back([&, writer] {
      for (unsigned i = 0; i < 200; ++i) {
        WriteBatch batch;
        if (!batch.Put("shared", "intermediate").ok() || !batch.Delete("shared").ok() ||
            !batch.Put("shared", "committed").ok() ||
            !batch.Put(std::to_string(writer), std::to_string(i)).ok() ||
            !db->Write({}, &batch).ok())
          ++failures;
      }
    });
  }
  for (auto& writer : writers) writer.join();
  done.store(true);
  for (auto& reader : readers) reader.join();
  EXPECT_EQ(failures.load(), 0U);
  db.reset();
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  EXPECT_GT(db->GetStats().level_file_counts.at(0), 1U);
  for (unsigned writer = 0; writer < 2; ++writer) {
    std::string value;
    ASSERT_TRUE(db->Get({}, std::to_string(writer), &value).ok());
    EXPECT_EQ(value, "199");
  }
}
}  // namespace
}  // namespace lsm
