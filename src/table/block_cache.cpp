#include "table/block_cache.hpp"

#include "util/hash.hpp"

namespace lsm::internal::table {

BlockCache::BlockCache(std::size_t capacity_bytes)
    : capacity_(capacity_bytes), shard_capacity_((capacity_bytes + kShards - 1) / kShards) {}

auto BlockCache::KeyHash::operator()(const Key& key) const noexcept -> std::size_t {
  std::array<char, 16> bytes{};
  for (unsigned i = 0; i < 8; ++i) {
    bytes[i] = static_cast<char>((key.file >> (8 * i)) & 0xffU);
    bytes[8 + i] = static_cast<char>((key.offset >> (8 * i)) & 0xffU);
  }
  return static_cast<std::size_t>(Hash64(std::string_view(bytes.data(), bytes.size())));
}

auto BlockCache::ShardOf(std::uint64_t file, std::uint64_t offset) -> std::size_t {
  // Use the high bits for the shard so they stay independent of the bucket
  // index, which the unordered_map takes from the low bits.
  const auto hash = static_cast<std::uint64_t>(KeyHash{}(Key{.file = file, .offset = offset}));
  return static_cast<std::size_t>(hash >> 60) % kShards;
}

auto BlockCache::ShardFor(const Key& key) -> Shard& {
  return shards_[ShardOf(key.file, key.offset)];
}

auto BlockCache::Lookup(std::uint64_t file, std::uint64_t offset) -> Block {
  const Key key{.file = file, .offset = offset};
  auto& shard = ShardFor(key);
  const std::lock_guard guard(shard.mutex);
  const auto found = shard.index.find(key);
  if (found == shard.index.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  shard.lru.splice(shard.lru.begin(), shard.lru, found->second);
  hits_.fetch_add(1, std::memory_order_relaxed);
  return found->second->block;
}

void BlockCache::Insert(std::uint64_t file, std::uint64_t offset, Block block) {
  if (!block || block->size() > shard_capacity_) {
    return;
  }
  const Key key{.file = file, .offset = offset};
  auto& shard = ShardFor(key);
  const std::lock_guard guard(shard.mutex);
  if (const auto found = shard.index.find(key); found != shard.index.end()) {
    shard.usage -= found->second->block->size();
    shard.lru.erase(found->second);
    shard.index.erase(found);
  }
  const auto charge = block->size();
  while (!shard.lru.empty() && shard.usage + charge > shard_capacity_) {
    const auto& victim = shard.lru.back();
    shard.usage -= victim.block->size();
    shard.index.erase(victim.key);
    shard.lru.pop_back();
    ++shard.evictions;
  }
  shard.lru.push_front({.key = key, .block = std::move(block)});
  shard.index.emplace(key, shard.lru.begin());
  shard.usage += charge;
  inserts_.fetch_add(1, std::memory_order_relaxed);
}

auto BlockCache::GetCounters() const -> Counters {
  Counters counters{.hits = hits_.load(std::memory_order_relaxed),
                    .misses = misses_.load(std::memory_order_relaxed),
                    .inserts = inserts_.load(std::memory_order_relaxed)};
  for (const auto& shard : shards_) {
    const std::lock_guard guard(shard.mutex);
    counters.usage += shard.usage;
    counters.entries += shard.index.size();
    counters.evictions += shard.evictions;
  }
  return counters;
}

}  // namespace lsm::internal::table
