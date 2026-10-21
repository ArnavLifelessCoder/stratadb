#include "memtable.hpp"

#include <algorithm>
#include <cstring>
#include <new>

#include "table/format.hpp"

namespace lsm::internal {

class MemTableIterator final : public InternalIterator {
 public:
  explicit MemTableIterator(std::shared_ptr<const MemTable> table) : table_(std::move(table)) {}
  [[nodiscard]] auto Valid() const -> bool override { return status_.ok() && node_ != nullptr; }
  void SeekToFirst() override { Set(table_->head_.next[0].load(std::memory_order_acquire)); }
  void SeekToLast() override {
    auto* node = &table_->head_;
    for (std::size_t level = MemTable::kHeight; level-- > 0;) {
      while (auto* next = node->next[level].load(std::memory_order_acquire)) node = next;
    }
    Set(node == &table_->head_ ? nullptr : node);
  }
  void Seek(std::string_view target) override {
    if (!status_.ok()) return;
    if (target.size() < 8) {
      status_ = Status::InvalidArgument("short memtable iterator seek");
      return;
    }
    Set(table_->Find(table::UserKey(target), table::Tag(target)));
  }
  void Next() override {
    if (Valid()) Set(node_->next[0].load(std::memory_order_acquire));
  }
  void Prev() override {
    if (!Valid()) return;
    std::array<MemTable::Node*, MemTable::kHeight> previous{};
    static_cast<void>(table_->Find(node_->key, node_->tag, &previous));
    Set(previous[0] == &table_->head_ ? nullptr : previous[0]);
  }
  [[nodiscard]] auto key() const -> std::string_view override {
    return Valid() ? key_ : std::string_view{};
  }
  [[nodiscard]] auto value() const -> std::string_view override {
    return Valid() ? node_->value : std::string_view{};
  }
  [[nodiscard]] auto status() const -> Status override { return status_; }

 private:
  void Set(const MemTable::Node* node) {
    node_ = node;
    key_.clear();
    if (node_ != nullptr) {
      key_ = node_->key;
      table::AppendFixed<8>(key_, node_->tag);
    }
  }
  std::shared_ptr<const MemTable> table_;
  const MemTable::Node* node_ = nullptr;
  std::string key_;
  Status status_;
};

auto MakeMemTableIterator(std::shared_ptr<const MemTable> table)
    -> std::unique_ptr<InternalIterator> {
  return std::make_unique<MemTableIterator>(std::move(table));
}

auto MemTable::Before(const Node& node, std::string_view key, std::uint64_t tag) -> bool {
  const int comparison = node.key.compare(key);
  return comparison < 0 || (comparison == 0 && node.tag > tag);
}

auto MemTable::Find(std::string_view key, std::uint64_t tag,
                    std::array<Node*, kHeight>* previous) const -> MemTable::Node* {
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

auto MemTable::Allocate(std::size_t bytes) -> void* {
  constexpr std::size_t alignment = alignof(Node);
  const auto padding =
      (alignment - (reinterpret_cast<std::uintptr_t>(free_) % alignment)) % alignment;
  if (remaining_ < padding || remaining_ - padding < bytes) {
    const auto size = std::max(std::size_t{64} * 1024, bytes);
    blocks_.emplace_back(size);
    free_ = blocks_.back().data();
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

auto MemTable::Insert(std::uint64_t sequence, WriteBatch::Type type, std::string_view key,
                      std::string_view value) -> Status {
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

auto MemTable::Get(std::string_view key, std::uint64_t sequence, std::string& value) const
    -> Status {
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

auto MemTable::Lookup(std::string_view key, std::uint64_t sequence, table::Entry& output) const
    -> Status {
  if (sequence > kMaxSequence) {
    return Status::InvalidArgument("sequence exceeds 56 bits");
  }
  const auto* node = Find(key, (sequence << 8) | 0xffU);
  if (node == nullptr || node->key != key) {
    return Status::NotFound("key");
  }
  table::Entry entry{.key = std::string(node->key), .value = std::string(node->value)};
  table::AppendFixed<8>(entry.key, node->tag);
  output = std::move(entry);
  return {};
}

auto MemTable::Entries() const -> std::vector<table::Entry> {
  std::vector<table::Entry> entries;
  for (auto* node = head_.next[0].load(std::memory_order_acquire); node != nullptr;
       node = node->next[0].load(std::memory_order_acquire)) {
    table::Entry entry{.key = std::string(node->key), .value = std::string(node->value)};
    table::AppendFixed<8>(entry.key, node->tag);
    entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace lsm::internal
