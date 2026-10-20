#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "lsm/db.hpp"
#include "temp_directory.hpp"
#include "version_set.hpp"

namespace lsm {
namespace {
TEST(CleanupTest, FailedObsoleteWalDeletionDoesNotPoisonWrites) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({.memtable_bytes = 1}, directory.path(), &db).ok());
  const auto old_wal = internal::WalPath(directory.path(), 2);
  const auto held = directory.path() / "held";
  std::filesystem::rename(old_wal, held);
  ASSERT_TRUE(std::filesystem::create_directory(old_wal));
  const auto blocker = old_wal / "blocker";
  ASSERT_TRUE(internal::WriteAtomicFile(blocker, "prevent removal").ok());
  ASSERT_TRUE(db->Put({true}, "first", "one").ok());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (db->GetStats().level_file_counts.at(0) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(db->GetStats().level_file_counts.at(0), 1U);
  ASSERT_TRUE(db->Put({true}, "second", "two").ok());
  db.reset();
  ASSERT_TRUE(std::filesystem::remove(blocker));
  ASSERT_TRUE(std::filesystem::remove(old_wal));
  std::filesystem::rename(held, old_wal);
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  EXPECT_FALSE(std::filesystem::exists(old_wal));
  std::string value;
  ASSERT_TRUE(db->Get({}, "first", &value).ok());
  EXPECT_EQ(value, "one");
  ASSERT_TRUE(db->Get({}, "second", &value).ok());
  EXPECT_EQ(value, "two");
}

TEST(CleanupTest, OpenReclaimsUnreferencedStorageFilesAndPreservesLiveFiles) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({.memtable_bytes = 1}, directory.path(), &db).ok());
  ASSERT_TRUE(db->Put({true}, "key", "value").ok());
  db.reset();
  for (const auto* name : {"99.wal", "100.sst", "101.wal.tmp", "102.sst.tmp", "CURRENT.tmp",
                           "MANIFEST-1.tmp", "WAL"}) {
    ASSERT_TRUE(internal::WriteAtomicFile(directory.path() / name, "unreferenced").ok());
  }
  ASSERT_TRUE(internal::WriteAtomicFile(directory.path() / "notes.txt", "unrelated").ok());
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  for (const auto* name : {"99.wal", "100.sst", "101.wal.tmp", "102.sst.tmp", "CURRENT.tmp",
                           "MANIFEST-1.tmp", "WAL"}) {
    EXPECT_FALSE(std::filesystem::exists(directory.path() / name)) << name;
  }
  EXPECT_TRUE(std::filesystem::exists(directory.path() / "notes.txt"));
  EXPECT_TRUE(std::filesystem::exists(directory.path() / "CURRENT"));
  EXPECT_TRUE(std::filesystem::exists(directory.path() / "MANIFEST-1"));
  EXPECT_TRUE(std::filesystem::exists(internal::WalPath(directory.path(), 3)));
  EXPECT_TRUE(std::filesystem::exists(internal::TablePath(directory.path(), 4)));
  std::string value;
  ASSERT_TRUE(db->Get({}, "key", &value).ok());
  EXPECT_EQ(value, "value");
}
}  // namespace
}  // namespace lsm
