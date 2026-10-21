#include "merging_iterator.hpp"

#include <algorithm>

#include "table/format.hpp"

namespace lsm::internal {
MergingIterator::MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children)
    : children_(std::move(children)) {
  for (const auto& child : children_) {
    if (!child->status().ok()) {
      status_ = child->status();
      break;
    }
  }
}

auto MergingIterator::Check(std::size_t child) -> bool {
  status_ = children_[child]->status();
  if (status_.ok() && children_[child]->Valid() &&
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
  const std::string current(key());
  if (reverse != reverse_) {
    for (std::size_t i = 0; i < children_.size(); ++i) {
      auto& child = children_[i];
      child->Seek(current);
      if (reverse) {
        if (!child->Valid() && child->status().ok())
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
  const auto later = [this](auto left, auto right) { return Later(left, right); };
  std::ranges::pop_heap(heap_, later);
  heap_.pop_back();
  auto& child = children_[selected];
  if (reverse)
    child->Prev();
  else
    child->Next();
  if (!Check(selected)) return;
  if (child->Valid()) {
    const auto order = table::Compare(current, child->key());
    if ((!reverse && order >= 0) || (reverse && order <= 0)) {
      status_ = Status::Corruption("merge child ordering");
      return;
    }
    heap_.push_back(selected);
    std::ranges::push_heap(heap_, later);
  }
}

void MergingIterator::Next() { Advance(false); }
void MergingIterator::Prev() { Advance(true); }
}  // namespace lsm::internal
