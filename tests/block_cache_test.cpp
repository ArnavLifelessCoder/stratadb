#include "table/block_cache.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace lsm::internal::table {
namespace {
auto Block(std::size_t size, char fill = 'x') -> BlockCache::Block {
  return std::make_shared<const std::string>(size, fill);
}

// Offsets of `count` keys in file 1 that all hash to `shard`.
auto OffsetsInShard(std::size_t shard, std::size_t count) -> std::vector<std::uint64_t> {
  std::vector<std::uint64_t> offsets;
  for (std::uint64_t offset = 0; offsets.size() < count; ++offset) {
    if (BlockCache::ShardOf(1, offset) == shard) offsets.push_back(offset);
  }
  return offsets;
}

TEST(BlockCacheTest, EvictsLeastRecentlyUsedWithinAShard) {
  BlockCache cache(BlockCache::kShards * 100);  // 100 bytes per shard.
  ASSERT_EQ(cache.ShardCapacity(), 100U);
  const auto offsets = OffsetsInShard(3, 4);
  cache.Insert(1, offsets[0], Block(40, 'a'));
  cache.Insert(1, offsets[1], Block(40, 'b'));
  // Touch the older entry so the newer one becomes least recently used.
  ASSERT_NE(cache.Lookup(1, offsets[0]), nullptr);
  cache.Insert(1, offsets[2], Block(40, 'c'));
  EXPECT_NE(cache.Lookup(1, offsets[0]), nullptr);
  EXPECT_EQ(cache.Lookup(1, offsets[1]), nullptr);
  EXPECT_NE(cache.Lookup(1, offsets[2]), nullptr);
  const auto counters = cache.GetCounters();
  EXPECT_EQ(counters.entries, 2U);
  EXPECT_EQ(counters.usage, 80U);
  EXPECT_EQ(counters.evictions, 1U);
  // One large block displaces everything else in its shard.
  cache.Insert(1, offsets[3], Block(100));
  EXPECT_EQ(cache.Lookup(1, offsets[0]), nullptr);
  EXPECT_EQ(cache.Lookup(1, offsets[2]), nullptr);
  EXPECT_EQ(cache.GetCounters().usage, 100U);
}

TEST(BlockCacheTest, ByteLimitHoldsUnderRandomInsertsAndLookups) {
  SCOPED_TRACE(test::seed);
  std::mt19937_64 random(test::seed);
  constexpr std::size_t kCapacity = 64 << 10;
  BlockCache cache(kCapacity);
  for (int i = 0; i < 20000; ++i) {
    const auto file = random() % 8;
    const auto offset = (random() % 512) * 4096;
    if (random() % 3 == 0) {
      static_cast<void>(cache.Lookup(file, offset));
    } else {
      cache.Insert(file, offset, Block(1 + random() % 6000));
    }
    const auto counters = cache.GetCounters();
    ASSERT_LE(counters.usage, kCapacity);
  }
  const auto counters = cache.GetCounters();
  EXPECT_GT(counters.evictions, 0U);
  EXPECT_GT(counters.hits, 0U);
  EXPECT_GT(counters.misses, 0U);
}

TEST(BlockCacheTest, EvictedBlockStaysValidForItsHolderAndOversizedBlocksAreSkipped) {
  BlockCache cache(BlockCache::kShards * 100);
  const auto offsets = OffsetsInShard(7, 2);
  cache.Insert(1, offsets[0], Block(60, 'k'));
  const auto held = cache.Lookup(1, offsets[0]);
  ASSERT_NE(held, nullptr);
  cache.Insert(1, offsets[1], Block(60, 'n'));  // Evicts offsets[0].
  EXPECT_EQ(cache.Lookup(1, offsets[0]), nullptr);
  EXPECT_EQ(*held, std::string(60, 'k'));
  // A block bigger than a shard is never cached.
  cache.Insert(2, 0, Block(101));
  EXPECT_EQ(cache.Lookup(2, 0), nullptr);
  // Replacing a key keeps one entry and recharges its size.
  cache.Insert(1, offsets[1], Block(10, 'r'));
  EXPECT_EQ(*cache.Lookup(1, offsets[1]), std::string(10, 'r'));
  EXPECT_EQ(cache.GetCounters().usage, 10U);
  // Zero capacity caches nothing.
  BlockCache disabled(0);
  disabled.Insert(1, 0, Block(1));
  EXPECT_EQ(disabled.Lookup(1, 0), nullptr);
}

TEST(BlockCacheTest, ConcurrentInsertsAndLookupsKeepTheLimitAndReturnTheRightBytes) {
  constexpr std::size_t kCapacity = 256 << 10;
  BlockCache cache(kCapacity);
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> threads;
  for (unsigned thread = 0; thread < 8; ++thread) {
    threads.emplace_back([&, thread] {
      std::mt19937_64 random(test::seed + thread);
      for (int i = 0; i < 20000; ++i) {
        const auto file = random() % 4;
        const auto offset = random() % 2048;
        // Content is a function of the key, so any hit can be checked.
        const auto fill = static_cast<char>('a' + (file * 31 + offset) % 26);
        if (auto block = cache.Lookup(file, offset)) {
          if (block->empty() || block->front() != fill) failures.fetch_add(1);
        } else {
          cache.Insert(file, offset, Block(64 + offset % 4000, fill));
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_LE(cache.GetCounters().usage, kCapacity);
}
}  // namespace
}  // namespace lsm::internal::table
