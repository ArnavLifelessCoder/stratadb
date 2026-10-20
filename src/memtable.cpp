#include "memtable.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace lsm::internal {

bool MemTable::Before(const Node& node, std::string_view key, std::uint64_t tag) {
  const int comparison = node.key.compare(key);
  return comparison < 0 || (comparison == 0 && node.tag > tag);
}

MemTable::Node* MemTable::Find(std::string_view key, std::uint64_t tag,
                               std::array<Node*, kHeight>* previous) const {
  Node* node = &head_;
  Node* next = nullptr;
  for (std::size_t level = kHeight; level-- > 0;) {
    next = node->next[level].load(std::memory_order_acquire);
    while (next != nullptr && Before(*next, key, tag)) {
      node = next;
      next = node->next[level].load(std::memory_order_acquire);
    }
    if (previous != nullptr) {
      (*previous)[level] = node;
    }
  }
  // Keep the candidate whose key was checked. Reloading the predecessor link
  // could return a newly inserted version newer than the reader's sequence.
  return next;
}

void* MemTable::Allocate(std::size_t bytes) {
  constexpr std::size_t alignment = alignof(Node);
  const auto padding =
      (alignment - (reinterpret_cast<std::uintptr_t>(free_) % alignment)) % alignment;
  if (remaining_ < padding || remaining_ - padding < bytes) {
    const auto size = std::max(std::size_t{64 * 1024}, bytes);
    blocks_.push_back(std::make_unique<std::byte[]>(size));
    free_ = blocks_.back().get();
    remaining_ = size;
    allocated_.fetch_add(size, std::memory_order_relaxed);
  } else {
    free_ += padding;
    remaining_ -= padding;
  }
  void* result = free_;
  free_ += bytes;
  remaining_ -= bytes;
  return result;
}

Status MemTable::Insert(std::uint64_t sequence, WriteBatch::Type type, std::string_view key,
                        std::string_view value) {
  if (sequence == 0 || sequence > kMaxSequence ||
      (type != WriteBatch::Type::kPut && type != WriteBatch::Type::kDelete)) {
    return Status::InvalidArgument("invalid internal key");
  }
  const auto tag = (sequence << 8) | static_cast<unsigned char>(type);
  std::array<Node*, kHeight> previous{};
  const auto* existing = Find(key, tag, &previous);
  if (existing != nullptr && existing->key == key && existing->tag == tag) {
    return Status::InvalidArgument("duplicate internal key");
  }
  if (type == WriteBatch::Type::kDelete) {
    value = {};
  }
  auto* node = new (Allocate(sizeof(Node) + key.size() + value.size())) Node;
  auto* bytes = reinterpret_cast<char*>(node + 1);
  if (!key.empty()) {
    std::memcpy(bytes, key.data(), key.size());
  }
  node->key = std::string_view(bytes, key.size());
  bytes += key.size();
  if (!value.empty()) {
    std::memcpy(bytes, value.data(), value.size());
  }
  node->value = std::string_view(bytes, value.size());
  node->tag = tag;
  std::size_t height = 1;
  while (height < kHeight && (random_() & 3U) == 0) {
    ++height;
  }
  // Publish level zero first. Once reachable, every immutable field is initialized.
  for (std::size_t level = 0; level < height; ++level) {
    node->next[level].store(previous[level]->next[level].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    previous[level]->next[level].store(node, std::memory_order_release);
  }
  return {};
}

Status MemTable::Get(std::string_view key, std::uint64_t sequence, std::string& value) const {
  if (sequence > kMaxSequence) {
    return Status::InvalidArgument("sequence exceeds 56 bits");
  }
  const auto* node = Find(key, (sequence << 8) | 0xffU);
  if (node == nullptr || node->key != key ||
      (node->tag & 0xffU) == static_cast<unsigned char>(WriteBatch::Type::kDelete)) {
    return Status::NotFound("key");
  }
  value.assign(node->value);
  return {};
}

}  // namespace lsm::internal
