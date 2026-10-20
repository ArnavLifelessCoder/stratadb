#include "compaction.hpp"

#include <algorithm>
#include <limits>

#include "table/format.hpp"

namespace lsm::internal {
auto PickCompaction(std::shared_ptr<const Version> version, const Options& options,
                    const CompactionPointers& pointers) -> std::optional<CompactionPlan> {
  std::array<std::vector<TableMetadata>, 7> levels;
  for (const auto& file : version->files) {
    levels.at(file.level).push_back(file);
  }
  std::uint32_t level = 0;
  if (levels[0].size() < static_cast<std::size_t>(options.l0_compaction_trigger)) {
    double best = 1.0;
    double target = 10.0 * 1024 * 1024;
    for (std::uint32_t candidate = 1; candidate < 6; ++candidate) {
      double bytes = 0;
      for (const auto& file : levels[candidate]) {
        bytes += static_cast<double>(file.size);
      }
      const double score = bytes / target;
      if (score >= best) {
        level = candidate;
        best = score;
      }
      target = target > std::numeric_limits<double>::max() / options.level_size_ratio
                   ? std::numeric_limits<double>::max()
                   : target * options.level_size_ratio;
    }
    if (level == 0) {
      return std::nullopt;
    }
  }
  CompactionPlan plan{.base = std::move(version), .level = level, .inputs = {}, .largest = {}};
  if (level == 0) {
    plan.inputs = levels[0];
  } else {
    auto& files = levels[level];
    std::ranges::sort(files, {}, &TableMetadata::smallest);
    auto chosen = std::ranges::find_if(
        files, [&](const auto& file) { return file.largest > pointers[level]; });
    if (chosen == files.end()) {
      chosen = files.begin();
    }
    plan.inputs.push_back(*chosen);
  }
  std::string smallest = plan.inputs.front().smallest;
  plan.largest = plan.inputs.front().largest;
  for (const auto& file : plan.inputs) {
    smallest = std::min(smallest, file.smallest);
    plan.largest = std::max(plan.largest, file.largest);
  }
  // Next-level ranges do not overlap one another, so one pass covers the
  // selected source range without repeatedly expanding into adjacent files.
  for (const auto& file : levels[level + 1]) {
    if (file.largest >= smallest && file.smallest <= plan.largest) {
      plan.inputs.push_back(file);
    }
  }
  return plan;
}

MergingIterator::MergingIterator(std::vector<std::vector<table::Entry>> streams)
    : streams_(std::move(streams)), positions_(streams_.size(), 0) {
  for (std::size_t source = 0; source < streams_.size(); ++source) {
    const auto& entries = streams_[source];
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const auto& entry = entries[index];
      if (!table::ValidKey(entry.key) ||
          ((table::Tag(entry.key) & 0xffU) == 0 && !entry.value.empty()) ||
          (index != 0 && table::Compare(entries[index - 1].key, entry.key) >= 0)) {
        status_ = Status::Corruption("invalid merge input");
        return;
      }
    }
    if (!entries.empty()) {
      heap_.push_back(source);
    }
  }
  std::ranges::make_heap(heap_, [this](auto left, auto right) { return Later(left, right); });
}

auto MergingIterator::Later(std::size_t left, std::size_t right) const -> bool {
  const int order =
      table::Compare(streams_[left][positions_[left]].key, streams_[right][positions_[right]].key);
  return order == 0 ? left > right : order > 0;
}

auto MergingIterator::entry() const -> const table::Entry& {
  return streams_[heap_.front()][positions_[heap_.front()]];
}

void MergingIterator::Next() {
  if (!Valid()) {
    return;
  }
  const auto later = [this](auto left, auto right) { return Later(left, right); };
  std::ranges::pop_heap(heap_, later);
  const auto source = heap_.back();
  heap_.pop_back();
  if (++positions_[source] != streams_[source].size()) {
    heap_.push_back(source);
    std::ranges::push_heap(heap_, later);
  }
}

auto CompactEntries(MergingIterator& input, const CompactionPlan& plan,
                    std::uint64_t oldest_snapshot, std::vector<table::Entry>& output) -> Status {
  if (!input.status().ok()) {
    return input.status();
  }
  std::vector<table::Entry> result;
  std::string user;
  std::uint64_t previous_sequence = kMaxSequence + 1;
  std::optional<table::Entry> previous;
  while (input.Valid()) {
    const auto& entry = input.entry();
    if (previous && previous->key == entry.key) {
      if (previous->value != entry.value) {
        return Status::Corruption("conflicting duplicate internal key");
      }
      input.Next();
      continue;
    }
    if (!previous || table::UserKey(entry.key) != user) {
      user = table::UserKey(entry.key);
      previous_sequence = kMaxSequence + 1;
    }
    const auto sequence = table::Tag(entry.key) >> 8;
    bool drop = previous_sequence <= oldest_snapshot;
    if (!drop && sequence <= oldest_snapshot && (table::Tag(entry.key) & 0xffU) == 0) {
      const bool deeper = std::ranges::any_of(plan.base->files, [&](const auto& file) {
        return file.level > plan.level + 1 && user >= file.smallest && user <= file.largest;
      });
      drop = !deeper;
    }
    if (!drop) {
      result.push_back(entry);
    }
    previous_sequence = sequence;
    previous = entry;
    input.Next();
  }
  if (!input.status().ok()) {
    return input.status();
  }
  output = std::move(result);
  return {};
}
}  // namespace lsm::internal
