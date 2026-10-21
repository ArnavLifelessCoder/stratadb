#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "internal_iterator.hpp"
#include "table/block.hpp"
#include "write_batch.hpp"

namespace lsm::internal {

// One externally serialized writer; readers need no mutex. Nodes live until destruction.
// Destruction requires all readers to have stopped.
class MemTable {
 public:
  explicit MemTable(std::uint64_t seed = 0x5eed) : random_(seed) { head_.next = head_next_.data(); }
  MemTable(const MemTable&) = delete;
  auto operator=(const MemTable&) -> MemTable& = delete;

  auto Insert(std::uint64_t sequence, WriteBatch::Type type, std::string_view key,
              std::string_view value) -> Status;
  auto Get(std::string_view key, std::uint64_t sequence, std::string& value) const -> Status;
  // Unlike Get, a visible tombstone is returned as an entry, not absence.
  auto Lookup(std::string_view key, std::uint64_t sequence, table::Entry& output) const -> Status;
  // Caller must prevent inserts while exporting (freeze the table or hold the writer lock).
  auto Entries() const -> std::vector<table::Entry>;
  [[nodiscard]] auto MemoryUsage() const noexcept -> std::size_t { return allocated_.load(); }

 private:
  friend class MemTableIterator;
  static constexpr std::size_t kHeight = 12;
  struct Node {
    std::string_view key;
    std::string_view value;
    // Numeric representation of the internal key's 56-bit sequence and 8-bit type.
    std::uint64_t tag = 0;
    // One link per level of this node, stored right after it in the arena, so
    // a node pays for its own height (1.33 levels on average), not for 12.
    std::atomic<Node*>* next = nullptr;
  };
  static auto Before(const Node& node, std::string_view key, std::uint64_t tag) -> bool;
  auto Find(std::string_view key, std::uint64_t tag,
            std::array<Node*, kHeight>* previous = nullptr) const -> Node*;
  auto Allocate(std::size_t bytes) -> void*;

  mutable Node head_;
  mutable std::array<std::atomic<Node*>, kHeight> head_next_{};
  std::vector<std::vector<std::byte>> blocks_;
  std::byte* free_ = nullptr;
  std::size_t remaining_ = 0;
  std::atomic<std::size_t> allocated_{0};
  std::mt19937_64 random_;
};

auto MakeMemTableIterator(std::shared_ptr<const MemTable> table)
    -> std::unique_ptr<InternalIterator>;

}  // namespace lsm::internal
