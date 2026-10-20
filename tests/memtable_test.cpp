#include "memtable.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace lsm::internal {
namespace {
TEST(MemTableTest, SequenceOrderingTombstonesBinaryKeysAndArenaOwnership) {
  MemTable table;
  std::string key("a\0b", 3);
  ASSERT_TRUE(table.Insert(3, WriteBatch::Type::kPut, key, "new").ok());
  ASSERT_TRUE(table.Insert(1, WriteBatch::Type::kPut, key, "old").ok());
  ASSERT_TRUE(table.Insert(2, WriteBatch::Type::kDelete, key, "").ok());
  key = "changed";
  std::string value = "sentinel";
  EXPECT_TRUE(table.Get(std::string("a\0b", 3), 0, value).IsNotFound());
  EXPECT_EQ(value, "sentinel");
  ASSERT_TRUE(table.Get(std::string("a\0b", 3), 1, value).ok());
  EXPECT_EQ(value, "old");
  EXPECT_TRUE(table.Get(std::string("a\0b", 3), 2, value).IsNotFound());
  ASSERT_TRUE(table.Get(std::string("a\0b", 3), kMaxSequence, value).ok());
  EXPECT_EQ(value, "new");
  ASSERT_TRUE(table.Insert(4, WriteBatch::Type::kPut, "", "").ok());
  ASSERT_TRUE(table.Get("", 4, value).ok());
  EXPECT_TRUE(value.empty());
  EXPECT_GT(table.MemoryUsage(), 0U);
  EXPECT_FALSE(table.Insert(0, WriteBatch::Type::kPut, "", "").ok());
  EXPECT_FALSE(table.Insert(4, WriteBatch::Type::kPut, "", "").ok());
  EXPECT_FALSE(table.Insert(kMaxSequence + 1, WriteBatch::Type::kPut, "", "").ok());
}

TEST(MemTableTest, RandomVersionsMatchMapReference) {
  SCOPED_TRACE(test::seed);
  MemTable table(test::seed);
  std::mt19937_64 random(test::seed);
  std::map<std::string, std::string> reference;
  for (std::uint64_t sequence = 1; sequence <= 10000; ++sequence) {
    const auto key = std::to_string(random() % 200);
    if (random() % 4 == 0) {
      ASSERT_TRUE(table.Insert(sequence, WriteBatch::Type::kDelete, key, {}).ok());
      reference.erase(key);
    } else {
      const auto value = std::to_string(random());
      ASSERT_TRUE(table.Insert(sequence, WriteBatch::Type::kPut, key, value).ok());
      reference[key] = value;
    }
    const auto lookup = std::to_string(random() % 200);
    std::string value;
    const auto status = table.Get(lookup, sequence, value);
    if (const auto found = reference.find(lookup); found != reference.end()) {
      ASSERT_TRUE(status.ok());
      EXPECT_EQ(value, found->second);
    } else {
      EXPECT_TRUE(status.IsNotFound());
    }
  }
}

TEST(MemTableTest, ConcurrentReadersObserveOnlyPublishedVersions) {
  MemTable table;
  std::atomic<std::uint64_t> published{0};
  std::atomic<bool> done{false};
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      do {
        const auto sequence = published.load(std::memory_order_acquire);
        std::string value;
        const auto status = table.Get("key", sequence, value);
        if (sequence == 0 ? !status.IsNotFound()
                          : !status.ok() || value != std::to_string(sequence)) {
          failures.fetch_add(1);
        }
      } while (!done.load());
    });
  }
  for (std::uint64_t sequence = 1; sequence <= 10000; ++sequence) {
    if (!table.Insert(sequence, WriteBatch::Type::kPut, "key", std::to_string(sequence)).ok()) {
      failures.fetch_add(1);
    }
    published.store(sequence, std::memory_order_release);
  }
  done.store(true);
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_EQ(failures.load(), 0U);
}
}  // namespace
}  // namespace lsm::internal
