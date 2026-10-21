#include "merging_iterator.hpp"

#include <algorithm>

#include "table/format.hpp"

namespace lsm::internal {
MergingIterator::MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children)
    : children_(std::move(children)) {
  for (const auto& child : children_) {
    if (!child->StatusRef().ok()) {
      status_ = child->StatusRef();
      break;
    }
  }
}

auto MergingIterator::Check(std::size_t child) -> bool {
  if (const auto& child_status = children_[child]->StatusRef(); !child_status.ok()) {
    status_ = child_status;
    return false;
  }
  if (children_[child]->Valid() &&
      (!table::ValidKey(children_[child]->key()) ||
       ((table::Tag(children_[child]->key()) & 0xffU) == 0 && !children_[child]->value().empty())))
    status_ = Status::Corruption("invalid merge cursor key or tombstone");
  return status_.ok();
}

auto MergingIterator::Later(std::size_t left, std::size_t right) const -> bool {
  const auto order = table::Compare(children_[left]->key(), children_[right]->key());
  return reverse_ ? (order == 0 ? left < right : order < 0)
                  : (order == 0 ? left > right : order > 0);
}

void MergingIterator::Rebuild(bool reverse) {
  reverse_ = reverse;
  heap_.clear();
  for (std::size_t child = 0; child < children_.size(); ++child) {
    if (!Check(child)) return;
    if (children_[child]->Valid()) heap_.push_back(child);
  }
  std::ranges::make_heap(heap_, [this](auto left, auto right) { return Later(left, right); });
}

void MergingIterator::SeekToFirst() {
  if (!status_.ok()) return;
  for (auto& child : children_) child->SeekToFirst();
  Rebuild(false);
}

void MergingIterator::SeekToLast() {
  if (!status_.ok()) return;
  for (auto& child : children_) child->SeekToLast();
  Rebuild(true);
}

void MergingIterator::Seek(std::string_view target) {
  if (!status_.ok()) return;
  for (auto& child : children_) child->Seek(target);
  Rebuild(false);
}

void MergingIterator::Advance(bool reverse) {
  if (!Valid()) return;
  const auto selected = heap_.front();
  current_.assign(key());
  const std::string_view current(current_);
  if (reverse != reverse_) {
    for (std::size_t i = 0; i < children_.size(); ++i) {
      auto& child = children_[i];
      child->Seek(current);
      if (reverse) {
        if (!child->Valid() && child->StatusRef().ok())
          child->SeekToLast();
        else if (child->Valid() && (table::Compare(child->key(), current) > 0 ||
                                    (child->key() == current && i >= selected)))
          child->Prev();
      } else if (child->Valid() && child->key() == current && i <= selected)
        child->Next();
      if (!Check(i)) return;
    }
    Rebuild(reverse);
    return;
  }
  auto& child = children_[selected];
  if (reverse)
    child->Prev();
  else
    child->Next();
  if (!Check(selected)) return;
  if (!child->Valid()) {
    std::ranges::pop_heap(heap_, [this](auto left, auto right) { return Later(left, right); });
    heap_.pop_back();
    return;
  }
  const auto order = table::Compare(current, child->key());
  if ((!reverse && order >= 0) || (reverse && order <= 0)) {
    status_ = Status::Corruption("merge child ordering");
    return;
  }
  // The advanced child is still at the root; sift it down once instead of a
  // pop and a push.
  for (std::size_t at = 0;;) {
    const auto left = 2 * at + 1;
    if (left >= heap_.size()) break;
    auto next = left;
    if (left + 1 < heap_.size() && Later(heap_[left], heap_[left + 1])) next = left + 1;
    if (!Later(heap_[at], heap_[next])) break;
    std::swap(heap_[at], heap_[next]);
    at = next;
  }
}

void MergingIterator::Next() { Advance(false); }
void MergingIterator::Prev() { Advance(true); }
}  // namespace lsm::internal
