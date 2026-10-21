#include "table/table_iterator.hpp"

#include <algorithm>

#include "table/format.hpp"

namespace lsm::internal::table {
auto TableIterator::Load(std::size_t index) -> bool {
  block_.reset();
  if (!status_.ok() || index >= reader_->index_.size()) return false;
  index_ = index;
  TableReader::BlockRef ref;
  status_ = reader_->LoadBlock(reader_->index_[index].value, ref);
  if (!status_.ok()) return false;
  block_owner_ = std::move(ref.owner);
  block_ = std::make_unique<BlockIterator>(ref.bytes, ref.verified);
  if (!block_->StatusRef().ok()) status_ = block_->StatusRef();
  return status_.ok();
}

void TableIterator::Check(bool at_last) {
  if (!block_->StatusRef().ok()) status_ = block_->StatusRef();
  if (!status_.ok()) return;
  if (!block_->Valid() || Compare(block_->key(), reader_->index_[index_].key) > 0 ||
      (at_last && block_->key() != reader_->index_[index_].key) ||
      (index_ > 0 && Compare(block_->key(), reader_->index_[index_ - 1].key) <= 0) ||
      ((Tag(block_->key()) & 0xffU) == 0 && !block_->value().empty())) {
    status_ = Status::Corruption("table iterator separator, ordering or tombstone");
  }
}

void TableIterator::SeekToFirst() {
  if (Load(0)) {
    block_->SeekToFirst();
    Check();
  }
}

void TableIterator::SeekToLast() {
  if (!reader_->index_.empty() && Load(reader_->index_.size() - 1)) {
    block_->SeekToLast();
    Check(true);
  }
}

void TableIterator::Seek(std::string_view target) {
  if (!status_.ok()) return;
  if (target.size() < 8) {
    status_ = Status::InvalidArgument("short table iterator seek");
    return;
  }
  const auto found = std::ranges::lower_bound(
      reader_->index_, target,
      [](std::string_view a, std::string_view b) { return Compare(a, b) < 0; }, &Entry::key);
  if (Load(static_cast<std::size_t>(found - reader_->index_.begin()))) {
    block_->Seek(target);
    Check();
  }
}

void TableIterator::Next() {
  if (!Valid()) return;
  const bool was_last = block_->key() == reader_->index_[index_].key;
  block_->Next();
  if (!block_->StatusRef().ok()) status_ = block_->StatusRef();
  if (!status_.ok()) return;
  if (!block_->Valid()) {
    if (!was_last) {
      status_ = Status::Corruption("table iterator short separator");
      return;
    }
    if (!Load(index_ + 1)) return;
    block_->SeekToFirst();
    Check();
    return;
  }
  // Within a block the block iterator has already checked that keys ascend,
  // and the block's last key must equal its separator (checked above when the
  // block ends), so the separator comparisons are only needed on a new block.
  if ((Tag(block_->key()) & 0xffU) == 0 && !block_->value().empty()) {
    status_ = Status::Corruption("table iterator tombstone with a value");
  }
}

void TableIterator::Prev() {
  if (!Valid()) return;
  block_->Prev();
  if (!block_->StatusRef().ok()) status_ = block_->StatusRef();
  if (!status_.ok()) return;
  if (!block_->Valid()) {
    if (index_ == 0) {
      block_.reset();
      return;
    }
    if (!Load(index_ - 1)) return;
    block_->SeekToLast();
    Check(true);
  } else
    Check();
}
}  // namespace lsm::internal::table
