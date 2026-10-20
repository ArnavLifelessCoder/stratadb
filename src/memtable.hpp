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

#include "write_batch.hpp"

namespace lsm::internal {

// One externally serialized writer; readers need no mutex. Nodes live until destruction.
// Destruction requires all readers to have stopped.
class MemTable {
 public:
  explicit MemTable(std::uint64_t seed = 0x5eed) : random_(seed) {}
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  Status Insert(std::uint64_t sequence, WriteBatch::Type type, std::string_view key,
                std::string_view value);
  Status Get(std::string_view key, std::uint64_t sequence, std::string& value) const;
  [[nodiscard]] std::size_t MemoryUsage() const noexcept { return allocated_.load(); }

 private:
  static constexpr std::size_t kHeight = 12;
  struct Node {
    std::string_view key;
    std::string_view value;
    // Numeric representation of the internal key's 56-bit sequence and 8-bit type.
    std::uint64_t tag = 0;
    std::array<std::atomic<Node*>, kHeight> next{};
  };
  static bool Before(const Node& node, std::string_view key, std::uint64_t tag);
  Node* Find(std::string_view key, std::uint64_t tag,
             std::array<Node*, kHeight>* previous = nullptr) const;
  void* Allocate(std::size_t bytes);

  mutable Node head_;
  std::vector<std::unique_ptr<std::byte[]>> blocks_;
  std::byte* free_ = nullptr;
  std::size_t remaining_ = 0;
  std::atomic<std::size_t> allocated_{0};
  std::mt19937_64 random_;
};

}  // namespace lsm::internal
