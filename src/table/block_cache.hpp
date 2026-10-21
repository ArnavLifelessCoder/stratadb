#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lsm::internal::table {

// Sharded LRU cache of CRC-checked data blocks, keyed by (file number, block
// offset). Capacity is in bytes and split evenly across 16 shards, each with its
// own mutex. Entries are shared_ptrs, so a block that a reader still holds stays
// valid after eviction; only the cache's reference is dropped. File numbers are
// never reused, so blocks of deleted tables simply age out.
class BlockCache {
 public:
  using Block = std::shared_ptr<const std::string>;
  static constexpr std::size_t kShards = 16;

  struct Counters {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t inserts = 0;
    std::uint64_t evictions = 0;
    std::size_t usage = 0;
    std::size_t entries = 0;
  };

  explicit BlockCache(std::size_t capacity_bytes);
  BlockCache(const BlockCache&) = delete;
  auto operator=(const BlockCache&) -> BlockCache& = delete;

  // Returns nullptr on a miss.
  auto Lookup(std::uint64_t file, std::uint64_t offset) -> Block;
  // A block larger than one shard's capacity is not cached. Inserting an
  // existing key replaces it.
  void Insert(std::uint64_t file, std::uint64_t offset, Block block);
  [[nodiscard]] auto Capacity() const -> std::size_t { return capacity_; }
  [[nodiscard]] auto ShardCapacity() const -> std::size_t { return shard_capacity_; }
  [[nodiscard]] auto GetCounters() const -> Counters;
  // Which shard holds a key; lets tests place keys deliberately.
  [[nodiscard]] static auto ShardOf(std::uint64_t file, std::uint64_t offset) -> std::size_t;

 private:
  struct Key {
    std::uint64_t file;
    std::uint64_t offset;
    auto operator==(const Key&) const -> bool = default;
  };
  struct KeyHash {
    auto operator()(const Key& key) const noexcept -> std::size_t;
  };
  struct Shard {
    struct Entry {
      Key key;
      Block block;
    };
    mutable std::mutex mutex;
    std::list<Entry> lru;  // Front is most recently used.
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index;
    std::size_t usage = 0;
    std::uint64_t evictions = 0;
  };
  auto ShardFor(const Key& key) -> Shard&;

  std::size_t capacity_;
  std::size_t shard_capacity_;
  std::array<Shard, kShards> shards_;
  std::atomic<std::uint64_t> hits_{0};
  std::atomic<std::uint64_t> misses_{0};
  std::atomic<std::uint64_t> inserts_{0};
};

}  // namespace lsm::internal::table
