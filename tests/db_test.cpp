#include "lsm/db.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <map>
#include <random>
#include <thread>

#include "platform/file.hpp"
#include "temp_directory.hpp"
#include "test_support.hpp"
#include "version_set.hpp"

namespace lsm {
namespace {
void CheckMap(DB& db, const std::map<std::string, std::string>& reference) {
  for (unsigned i = 0; i < 32; ++i) {
    const std::string key(1, static_cast<char>(i));
    std::string value = "unchanged";
    const auto status = db.Get({}, key, &value);
    if (const auto found = reference.find(key); found != reference.end()) {
      ASSERT_TRUE(status.ok()) << status.ToString();
      ASSERT_EQ(value, found->second);
    } else {
      ASSERT_TRUE(status.IsNotFound()) << status.ToString();
      ASSERT_EQ(value, "unchanged");
    }
  }
}

TEST(DBTest, TenThousandSeedStreamsMatchMapAndCleanReplay) {
  constexpr unsigned seeds = 10000;
  constexpr unsigned operations = 64;
  std::cout << "DB differential: " << seeds << " seeds, " << operations
            << " operations per seed, base seed " << test::seed << '\n';
  // Reuse each DB for 100 independent seeded streams to keep filesystem setup
  // bounded. The reference carries state across streams; every stream checks
  // all possible keys, including deleted and never-written keys.
  for (unsigned group = 0; group < seeds / 100; ++group) {
    test::TempDirectory directory;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
    std::map<std::string, std::string> reference;
    for (unsigned stream = 0; stream < 100; ++stream) {
      const auto seed = test::seed + group * 100 + stream;
      SCOPED_TRACE(::testing::Message() << "stream_seed=" << seed);
      std::mt19937_64 random(seed);
      for (unsigned operation = 0; operation < operations; ++operation) {
        SCOPED_TRACE(operation);
        const std::string key(1, static_cast<char>(random() % 32));
        const auto choice = random() % 4;
        std::string value(static_cast<std::size_t>(random() % 40), '\0');
        for (auto& byte : value) {
          byte = static_cast<char>(random());
        }
        if (choice == 0) {
          ASSERT_TRUE(db->Put({}, key, value).ok());
          reference[key] = value;
        } else if (choice == 1) {
          ASSERT_TRUE(db->Delete({}, key).ok());
          reference.erase(key);
        } else if (choice == 2) {
          WriteBatch batch;
          ASSERT_TRUE(batch.Put(key, "intermediate").ok());
          ASSERT_TRUE(batch.Delete(key).ok());
          ASSERT_TRUE(batch.Put(key, value).ok());
          const std::string other(1, static_cast<char>(random() % 32));
          ASSERT_TRUE(batch.Delete(other).ok());
          ASSERT_TRUE(db->Write({}, &batch).ok());
          reference[key] = value;
          reference.erase(other);
        }
        const std::string lookup(1, static_cast<char>(random() % 32));
        std::string actual;
        const auto status = db->Get({}, lookup, &actual);
        if (reference.contains(lookup)) {
          ASSERT_TRUE(status.ok());
          ASSERT_EQ(actual, reference.at(lookup));
        } else {
          ASSERT_TRUE(status.IsNotFound());
        }
      }
      CheckMap(*db, reference);
    }
    db.reset();
    ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
    CheckMap(*db, reference);
    ASSERT_TRUE(db->Put({true}, "after-replay", "durable").ok());
    db.reset();
    ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
    std::string value;
    ASSERT_TRUE(db->Get({}, "after-replay", &value).ok());
    EXPECT_EQ(value, "durable");
  }
}

TEST(DBTest, EmptyBinaryLargeValuesBatchOrderStatsAndExclusiveOpen) {
  test::TempDirectory directory;
  const auto path = directory.path() / std::filesystem::path(u8"数据库");
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, path, &db).ok());
  std::unique_ptr<DB> duplicate;
  EXPECT_EQ(DB::Open({}, path, &duplicate).code(), StatusCode::kIOError);
  EXPECT_EQ(duplicate, nullptr);
  EXPECT_FALSE(DB::Open({}, path, nullptr).ok());
  EXPECT_FALSE(DB::Open({}, {}, &duplicate).ok());
  EXPECT_FALSE(db->Write({}, nullptr).ok());
  EXPECT_FALSE(db->Get({}, "", nullptr).ok());
  WriteBatch empty;
  ASSERT_TRUE(db->Write({true}, &empty).ok());
  EXPECT_EQ(db->GetStats().wal_syncs, 0U);
  ASSERT_TRUE(db->Put({true}, "", "").ok());
  const std::string key("a\0b", 3);
  const std::string big(1024 * 1024, '\xff');
  ASSERT_TRUE(db->Put({true}, key, big).ok());
  ASSERT_TRUE(db->Delete({true}, key).ok());
  WriteBatch batch;
  ASSERT_TRUE(batch.Put(key, "first").ok());
  ASSERT_TRUE(batch.Delete(key).ok());
  ASSERT_TRUE(batch.Put(key, big).ok());
  ASSERT_TRUE(db->Write({true}, &batch).ok());
  const auto stats = db->GetStats();
  EXPECT_EQ(stats.wal_syncs, 4U);
  EXPECT_GT(stats.disk_bytes_written, stats.user_bytes_written);
  EXPECT_EQ(stats.level_file_counts, std::vector<std::uint64_t>{0});
  EXPECT_GT(stats.memtable_bytes, big.size());
  db.reset();
  ASSERT_TRUE(DB::Open({}, path, &db).ok());
  std::string value;
  ASSERT_TRUE(db->Get({}, key, &value).ok());
  EXPECT_EQ(value, big);
  ASSERT_TRUE(db->Get({}, "", &value).ok());
  EXPECT_TRUE(value.empty());
}

TEST(DBTest, TornTailOnDiskIsTruncatedAndMiddleCorruptionFailsOpen) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  ASSERT_TRUE(db->Put({true}, "key", "value").ok());
  db.reset();
  std::unique_ptr<platform::File> file;
  ASSERT_TRUE(platform::File::Open(internal::WalPath(directory.path(), 2), false, file).ok());
  ASSERT_TRUE(file->Append("torn").ok());
  ASSERT_TRUE(file->Sync().ok());
  file.reset();
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  EXPECT_EQ(db->GetStats().recovered_tail_bytes, 4U);
  ASSERT_TRUE(db->Put({true}, "other", "value").ok());
  db.reset();
  ASSERT_TRUE(platform::File::Open(internal::WalPath(directory.path(), 2), false, file).ok());
  std::string bytes;
  ASSERT_TRUE(file->ReadAll(bytes).ok());
  bytes[0] ^= 1;
  ASSERT_TRUE(file->Truncate(0).ok());
  ASSERT_TRUE(file->Append(bytes).ok());
  ASSERT_TRUE(file->Sync().ok());
  file.reset();
  EXPECT_EQ(DB::Open({}, directory.path(), &db).code(), StatusCode::kCorruption);
  EXPECT_EQ(db, nullptr);
  EXPECT_EQ(std::filesystem::file_size(internal::WalPath(directory.path(), 2)), bytes.size());
}

TEST(DBTest, ConcurrentWritersAndReadersNeverExposePartialBatches) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  ASSERT_TRUE(db->Put({}, "shared", "committed").ok());
  std::atomic<bool> done{false};
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!done.load()) {
        std::string value;
        if (!db->Get({}, "shared", &value).ok() || value != "committed") {
          ++failures;
        }
      }
    });
  }
  std::vector<std::thread> writers;
  for (int i = 0; i < 4; ++i) {
    writers.emplace_back([&, i] {
      for (int j = 0; j < 1000; ++j) {
        WriteBatch batch;
        if (!batch.Put("shared", "uncommitted").ok() || !batch.Delete("shared").ok() ||
            !batch.Put("shared", "committed").ok() ||
            !batch.Put(std::to_string(i), std::to_string(j)).ok() || !db->Write({}, &batch).ok()) {
          ++failures;
        }
      }
    });
  }
  for (auto& writer : writers) {
    writer.join();
  }
  done.store(true);
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_EQ(failures.load(), 0U);
  db.reset();
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  for (int i = 0; i < 4; ++i) {
    std::string value;
    ASSERT_TRUE(db->Get({}, std::to_string(i), &value).ok());
    EXPECT_EQ(value, "999");
  }
}
}  // namespace
}  // namespace lsm
