#pragma once

#include <memory>
#include <vector>

#include "internal_iterator.hpp"

namespace lsm::internal {
// A heap of child cursors. Construction performs no positioning or data reads.
class MergingIterator final : public InternalIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children);
  [[nodiscard]] auto Valid() const -> bool override { return status_.ok() && !heap_.empty(); }
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(std::string_view target) override;
  void Next() override;
  void Prev() override;
  [[nodiscard]] auto key() const -> std::string_view override {
    return Valid() ? children_[heap_.front()]->key() : std::string_view{};
  }
  [[nodiscard]] auto value() const -> std::string_view override {
    return Valid() ? children_[heap_.front()]->value() : std::string_view{};
  }
  [[nodiscard]] auto status() const -> Status override { return status_; }

 private:
  auto Check(std::size_t child) -> bool;
  [[nodiscard]] auto Later(std::size_t left, std::size_t right) const -> bool;
  void Rebuild(bool reverse);
  void Advance(bool reverse);
  std::vector<std::unique_ptr<InternalIterator>> children_;
  std::vector<std::size_t> heap_;
  bool reverse_ = false;
  Status status_;
};
}  // namespace lsm::internal
