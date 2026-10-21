#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "internal_iterator.hpp"
#include "table/block.hpp"
#include "table/format.hpp"

namespace lsm::test {
// Deliberately simple, materialized reference used only in iterator tests.
class VectorIterator final : public internal::InternalIterator {
 public:
  explicit VectorIterator(std::vector<internal::table::Entry> entries)
      : entries_(std::move(entries)), position_(entries_.size()) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (!internal::table::ValidKey(entries_[i].key) ||
          (i != 0 && internal::table::Compare(entries_[i - 1].key, entries_[i].key) >= 0)) {
        status_ = Status::Corruption("invalid reference iterator input");
        break;
      }
    }
  }
  [[nodiscard]] auto Valid() const -> bool override {
    return status_.ok() && position_ < entries_.size();
  }
  void SeekToFirst() override { position_ = 0; }
  void SeekToLast() override { position_ = entries_.empty() ? 0 : entries_.size() - 1; }
  void Seek(std::string_view target) override {
    const auto it = std::ranges::lower_bound(
        entries_, target,
        [](const auto& a, const auto& b) { return internal::table::Compare(a, b) < 0; },
        &internal::table::Entry::key);
    position_ = static_cast<std::size_t>(it - entries_.begin());
  }
  void Next() override {
    if (Valid()) ++position_;
  }
  void Prev() override {
    if (Valid()) position_ = position_ == 0 ? entries_.size() : position_ - 1;
  }
  [[nodiscard]] auto key() const -> std::string_view override {
    return Valid() ? entries_[position_].key : std::string_view{};
  }
  [[nodiscard]] auto value() const -> std::string_view override {
    return Valid() ? entries_[position_].value : std::string_view{};
  }
  [[nodiscard]] auto status() const -> Status override { return status_; }

 private:
  std::vector<internal::table::Entry> entries_;
  std::size_t position_;
  Status status_;
};

inline auto MakeIterators(std::vector<std::vector<internal::table::Entry>> streams)
    -> std::vector<std::unique_ptr<internal::InternalIterator>> {
  std::vector<std::unique_ptr<internal::InternalIterator>> result;
  for (auto& stream : streams)
    result.push_back(std::make_unique<VectorIterator>(std::move(stream)));
  return result;
}
}  // namespace lsm::test
