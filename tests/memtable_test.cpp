#include "memtable.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "table/format.hpp"
#include "test_support.hpp"

namespace lsm::internal {
namespace {
TEST(MemTableIteratorTest, NavigationSeeksAndOwnershipMatchExportedReference) {
  auto table = std::make_shared<MemTable>();
  for (std::uint64_t i = 0; i < 1000; ++i)
    ASSERT_TRUE(table
                    ->Insert(i + 1, WriteBatch::Type::kPut, std::to_string(10000 + i / 4),
                             std::to_string(i))
                    .ok());
  const auto entries = table->Entries();
  const std::weak_ptr<const MemTable> weak = table;
  auto it = MakeMemTableIterator(table);
  EXPECT_FALSE(it->Valid());
  table.reset();
  EXPECT_FALSE(weak.expired());
  it->SeekToFirst();
  for (const auto& entry : entries) {
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), entry.key);
    EXPECT_EQ(it->value(), entry.value);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());
  it->SeekToLast();
  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), entry->key);
    EXPECT_EQ(it->value(), entry->value);
    it->Prev();
  }
  EXPECT_FALSE(it->Valid());
  for (std::size_t i = 1; i + 1 < entries.size(); ++i) {
    it->Seek(entries[i].key);
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key(), entries[i].key);
    it->Prev();
    EXPECT_EQ(it->key(), entries[i - 1].key);
    it->Next();
    EXPECT_EQ(it->key(), entries[i].key);
  }
  EXPECT_TRUE(it->status().ok());
  it.reset();
  EXPECT_TRUE(weak.expired());
}

TEST(MemTableTest, FrozenExportAndLookupPreserveVersionsAndTombstones) {
  MemTable table;
  ASSERT_TRUE(table.Insert(1, WriteBatch::Type::kPut, "z", "old").ok());
  ASSERT_TRUE(table.Insert(2, WriteBatch::Type::kPut, "", "").ok());
  ASSERT_TRUE(table.Insert(3, WriteBatch::Type::kDelete, "z", "").ok());
  const auto entries = table.Entries();
  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].key, table::InternalKey("", 2, WriteBatch::Type::kPut));
  EXPECT_EQ(entries[1].key, table::InternalKey("z", 3, WriteBatch::Type::kDelete));
  EXPECT_EQ(entries[2].value, "old");
  table::Entry entry;
  ASSERT_TRUE(table.Lookup("z", 3, entry).ok());
  EXPECT_EQ(entry, entries[1]);
  ASSERT_TRUE(table.Lookup("z", 1, entry).ok());
  EXPECT_EQ(entry, entries[2]);
  EXPECT_TRUE(table.Lookup("z", 0, entry).IsNotFound());
  EXPECT_EQ(entry, entries[2]);
}

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

TEST(MemTableTest, ConcurrentReadersTraverseManyKeysAndRetainOlderSnapshots) {
  constexpr std::uint64_t kKeys = 64;
  constexpr std::uint64_t kRounds = 256;
  MemTable table(test::seed);
  std::atomic<std::uint64_t> published{0};
  std::atomic<bool> done{false};
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> readers;
  for (std::uint64_t reader = 0; reader < 4; ++reader) {
    readers.emplace_back([&, reader] {
      std::mt19937_64 random(test::seed + reader);
      do {
        const auto current = published.load(std::memory_order_acquire);
        const auto round = current == 0 ? 0 : random() % (current + 1);
        for (std::uint64_t key = 0; key < kKeys; ++key) {
          std::string value = "unchanged";
          const auto status = table.Get(std::to_string(key), round * kKeys, value);
          const bool deleted = round == 0 || (round + key) % 5 == 0;
          if (deleted ? !status.IsNotFound() || value != "unchanged"
                      : !status.ok() || value != std::to_string(round)) {
            failures.fetch_add(1);
          }
        }
      } while (!done.load());
    });
  }
  for (std::uint64_t round = 1; round <= kRounds; ++round) {
    for (std::uint64_t index = 0; index < kKeys; ++index) {
      // Permute insertion order to exercise links before and after existing nodes.
      const auto key = (index * 17) % kKeys;
      const auto type = (round + key) % 5 == 0 ? WriteBatch::Type::kDelete : WriteBatch::Type::kPut;
      if (!table
               .Insert((round - 1) * kKeys + index + 1, type, std::to_string(key),
                       type == WriteBatch::Type::kPut ? std::to_string(round) : "")
               .ok()) {
        failures.fetch_add(1);
      }
    }
    published.store(round, std::memory_order_release);
  }
  done.store(true);
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_EQ(failures.load(), 0U);
}
}  // namespace
}  // namespace lsm::internal
