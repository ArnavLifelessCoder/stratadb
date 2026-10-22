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
  // Every level gets a score and the highest one at or above 1 is compacted:
  // L0 by table count over the trigger, L1+ by size over the level target. L0
  // must not win just by reaching its trigger: under a steady write load it
  // is almost always there, and L1 would then grow without bound and make
  // every L0 compaction rewrite nearly all of it.
  std::uint32_t level = 0;
  double best = static_cast<double>(levels[0].size()) /
                static_cast<double>(std::max(options.l0_compaction_trigger, 1));
  double target = 10.0 * 1024 * 1024;
  for (std::uint32_t candidate = 1; candidate < 6; ++candidate) {
    double bytes = 0;
    for (const auto& file : levels[candidate]) {
      bytes += static_cast<double>(file.size);
    }
    const double score = bytes / target;
    if (score > best) {
      level = candidate;
      best = score;
    }
    target = target > std::numeric_limits<double>::max() / options.level_size_ratio
                 ? std::numeric_limits<double>::max()
                 : target * options.level_size_ratio;
  }
  if (best < 1.0) {
    return std::nullopt;
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

auto CompactEntries(MergingIterator& input, const CompactionPlan& plan,
                    std::uint64_t oldest_snapshot,
                    const std::function<Status(const table::Entry&)>& emit) -> Status {
  input.SeekToFirst();
  if (!input.status().ok()) {
    return input.status();
  }
  std::string user;
  std::uint64_t previous_sequence = kMaxSequence + 1;
  std::optional<table::Entry> previous;
  while (input.Valid()) {
    const table::Entry entry{.key = std::string(input.key()), .value = std::string(input.value())};
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
      const auto status = emit(entry);
      if (!status.ok()) return status;
    }
    previous_sequence = sequence;
    previous = entry;
    input.Next();
  }
  if (!input.status().ok()) {
    return input.status();
  }
  return {};
}
auto CompactEntries(MergingIterator& input, const CompactionPlan& plan,
                    std::uint64_t oldest_snapshot, std::vector<table::Entry>& output) -> Status {
  std::vector<table::Entry> result;
  const auto status = CompactEntries(input, plan, oldest_snapshot, [&](const table::Entry& entry) {
    result.push_back(entry);
    return Status{};
  });
  if (status.ok()) output = std::move(result);
  return status;
}
}  // namespace lsm::internal
