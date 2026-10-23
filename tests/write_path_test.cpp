#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "compaction.hpp"
#include "lsm/db.hpp"
#include "temp_directory.hpp"
#include "test_support.hpp"

namespace lsm {
namespace {
template <typename Predicate>
auto WaitFor(Predicate predicate) -> bool {
  for (int i = 0; i < 1000; ++i) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

// Restores background compaction even when an assertion fails mid-test.
class PauseCompactions {
 public:
  PauseCompactions() { internal::pause_compactions_for_testing.store(true); }
  ~PauseCompactions() { internal::pause_compactions_for_testing.store(false); }
  PauseCompactions(const PauseCompactions&) = delete;
  auto operator=(const PauseCompactions&) -> PauseCompactions& = delete;
};

auto L0(DB& db) -> std::uint64_t { return db.GetStats().level_file_counts.at(0); }

TEST(WritePathTest, SixteenSyncWritersShareFewerSyncsThanWrites) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  constexpr unsigned kThreads = 16;
  constexpr unsigned kWrites = 100;
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> threads;
  for (unsigned thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&, thread] {
      for (unsigned i = 0; i < kWrites; ++i) {
        const auto key = std::to_string(thread) + "/" + std::to_string(i);
        if (!db->Put({.sync = true}, key, "value-" + key).ok()) failures.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  ASSERT_EQ(failures.load(), 0U);
  const auto stats = db->GetStats();
  std::cout << "GROUP_COMMIT writes=" << stats.write_calls << " wal_records=" << stats.wal_records
            << " wal_syncs=" << stats.wal_syncs << '\n';
  EXPECT_EQ(stats.write_calls, kThreads * kWrites);
  EXPECT_LT(stats.wal_syncs, stats.write_calls);
  EXPECT_LT(stats.wal_records, stats.write_calls);
  // Every synced group is one record and one sync.
  EXPECT_EQ(stats.wal_syncs, stats.wal_records);
  db.reset();
  ASSERT_TRUE(DB::Open({}, directory.path(), &db).ok());
  for (unsigned thread = 0; thread < kThreads; ++thread) {
    for (unsigned i = 0; i < kWrites; ++i) {
      const auto key = std::to_string(thread) + "/" + std::to_string(i);
      std::string value;
      ASSERT_TRUE(db->Get({}, key, &value).ok()) << key;
      EXPECT_EQ(value, "value-" + key);
    }
  }
}

TEST(WritePathTest, MixedSyncAndAsyncWritersAndBatchesAllCommit) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({.memtable_bytes = 32 << 10}, directory.path(), &db).ok());
  constexpr unsigned kThreads = 8;
  constexpr unsigned kWrites = 300;
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> threads;
  for (unsigned thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&, thread] {
      for (unsigned i = 0; i < kWrites; ++i) {
        const auto prefix = std::to_string(thread) + "/";
        WriteBatch batch;
        // Both keys of a batch must always carry the same counter.
        if (!batch.Put(prefix + "a", std::to_string(i)).ok() ||
            !batch.Put(prefix + "b", std::to_string(i)).ok() ||
            !db->Write({.sync = (i + thread) % 3 == 0}, &batch).ok()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  std::atomic<bool> done{false};
  std::thread reader([&] {
    while (!done.load()) {
      const auto snapshot = db->GetSnapshot();
      for (unsigned thread = 0; thread < kThreads; ++thread) {
        const auto prefix = std::to_string(thread) + "/";
        std::string a;
        std::string b;
        const auto sa = db->Get({.snapshot = snapshot}, prefix + "a", &a);
        const auto sb = db->Get({.snapshot = snapshot}, prefix + "b", &b);
        if (sa.ok() != sb.ok() || a != b) failures.fetch_add(1);
      }
      db->ReleaseSnapshot(snapshot);
    }
  });
  for (auto& thread : threads) thread.join();
  done.store(true);
  reader.join();
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(db->GetStats().write_calls, kThreads * kWrites);
  for (unsigned thread = 0; thread < kThreads; ++thread) {
    std::string value;
    ASSERT_TRUE(db->Get({}, std::to_string(thread) + "/a", &value).ok());
    EXPECT_EQ(value, std::to_string(kWrites - 1));
  }
}

TEST(WritePathTest, WritesStopAtTheL0LimitUntilCompactionCatchesUp) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  // Every write rotates its memtable, so each write adds one L0 table.
  const Options options{.memtable_bytes = 1, .l0_compaction_trigger = 2, .l0_stop_writes = 4};
  ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
  constexpr unsigned kWrites = 12;
  std::atomic<unsigned> completed{0};
  std::thread writer;
  {
    const PauseCompactions pause;
    writer = std::thread([&] {
      for (unsigned i = 0; i < kWrites; ++i) {
        if (!db->Put({}, std::to_string(i), "value").ok()) return;
        completed.fetch_add(1);
      }
    });
    ASSERT_TRUE(WaitFor([&] { return db->GetStats().stopped_writes > 0; }));
    // The writer is parked at the limit and makes no progress while paused.
    const auto parked = completed.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(completed.load(), parked);
    EXPECT_LT(parked, kWrites);
    // The check runs before a write; the flush of the previous write may still
    // land, so L0 parks at the limit or one table above it.
    EXPECT_GE(L0(*db), 4U);
    EXPECT_LE(L0(*db), 5U);
  }
  writer.join();
  EXPECT_EQ(completed.load(), kWrites);
  const auto stats = db->GetStats();
  EXPECT_GT(stats.stall_micros, 150000U);
  EXPECT_GE(stats.stopped_writes, 1U);
  for (unsigned i = 0; i < kWrites; ++i) {
    std::string value;
    ASSERT_TRUE(db->Get({}, std::to_string(i), &value).ok());
  }
  EXPECT_TRUE(WaitFor([&] { return L0(*db) < 4; }));
}

TEST(WritePathTest, WritesSlowDownFromEightL0TablesBeforeStopping) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({.memtable_bytes = 1}, directory.path(), &db).ok());  // Stop at 12.
  const PauseCompactions pause;
  for (unsigned i = 0; i < 10; ++i) {
    ASSERT_TRUE(db->Put({}, std::to_string(i), "value").ok());
    ASSERT_TRUE(WaitFor([&] { return L0(*db) == i + 1; }));
  }
  const auto stats = db->GetStats();
  // Writes 9 and 10 found 8 and 9 tables in L0.
  EXPECT_EQ(stats.slowed_writes, 2U);
  EXPECT_EQ(stats.stopped_writes, 0U);
  EXPECT_GE(stats.stall_micros, 2000U);
}

TEST(WritePathTest, StatsCountFlushesCompactionsAndBytesPerLevel) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(
      DB::Open({.memtable_bytes = 1, .l0_compaction_trigger = 2}, directory.path(), &db).ok());
  for (unsigned i = 0; i < 8; ++i) {
    ASSERT_TRUE(db->Put({}, std::to_string(i), std::string(100, 'v')).ok());
  }
  ASSERT_TRUE(WaitFor([&] {
    const auto stats = db->GetStats();
    return stats.flushes >= 4 && stats.compactions >= 1;
  }));
  const auto stats = db->GetStats();
  ASSERT_EQ(stats.level_bytes.size(), stats.level_file_counts.size());
  std::uint64_t tables = 0;
  for (std::size_t level = 0; level < stats.level_bytes.size(); ++level) {
    // A level has bytes exactly when it has tables.
    EXPECT_EQ(stats.level_bytes[level] > 0, stats.level_file_counts[level] > 0) << level;
    tables += stats.level_file_counts[level];
  }
  EXPECT_GT(tables, 0U);
}

TEST(WritePathTest, BlockCacheServesRepeatedReadsWithinItsByteLimit) {
  for (const std::size_t capacity : {std::size_t{0}, std::size_t{16} << 10, std::size_t{8} << 20}) {
    SCOPED_TRACE(capacity);
    test::TempDirectory directory;
    std::unique_ptr<DB> db;
    const Options options{.memtable_bytes = 64 << 10, .block_cache_bytes = capacity};
    ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
    for (unsigned i = 0; i < 4000; ++i) {
      ASSERT_TRUE(db->Put({}, std::to_string(100000 + i), std::string(100, 'v')).ok());
    }
    ASSERT_TRUE(WaitFor([&] {
      std::uint64_t tables = 0;
      for (const auto count : db->GetStats().level_file_counts) tables += count;
      return tables > 0;
    }));
    db.reset();
    ASSERT_TRUE(DB::Open(options, directory.path(), &db).ok());
    for (int pass = 0; pass < 2; ++pass) {
      for (unsigned i = 0; i < 4000; i += 7) {
        std::string value;
        ASSERT_TRUE(db->Get({}, std::to_string(100000 + i), &value).ok());
        ASSERT_EQ(value, std::string(100, 'v'));
      }
    }
    const auto stats = db->GetStats();
    EXPECT_LE(stats.block_cache_bytes, capacity);
    if (capacity == 0) {
      EXPECT_EQ(stats.block_cache_hits + stats.block_cache_misses, 0U);
    } else if (capacity >= (1U << 20)) {
      // Everything fits, so the second pass is served from the cache.
      EXPECT_GT(stats.block_cache_hits, stats.block_cache_misses);
    } else {
      EXPECT_GT(stats.block_cache_misses, 0U);
    }
  }
}
}  // namespace
}  // namespace lsm
