#pragma once

#include "table/block_iterator.hpp"
#include "table/table.hpp"

namespace lsm::internal::table {
// Creation pins the reader but performs no block reads. Each block transition
// or seek loads and CRC-checks its data block, including revisits.
class TableIterator final : public InternalIterator {
 public:
  explicit TableIterator(std::shared_ptr<const TableReader> reader) : reader_(std::move(reader)) {}
  [[nodiscard]] auto Valid() const -> bool override {
    return status_.ok() && block_ && block_->Valid();
  }
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(std::string_view target) override;
  void Next() override;
  void Prev() override;
  [[nodiscard]] auto key() const -> std::string_view override {
    return Valid() ? block_->key() : std::string_view{};
  }
  [[nodiscard]] auto value() const -> std::string_view override {
    return Valid() ? block_->value() : std::string_view{};
  }
  [[nodiscard]] auto StatusRef() const -> const Status& override { return status_; }

 private:
  auto Load(std::size_t index) -> bool;
  void Check(bool at_last = false);
  std::shared_ptr<const TableReader> reader_;
  BlockCache::Block block_owner_;  // Keeps a cached block alive while block_ reads it.
  std::unique_ptr<BlockIterator> block_;
  std::size_t index_ = 0;
  Status status_;
};
}  // namespace lsm::internal::table
