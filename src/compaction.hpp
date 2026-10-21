#pragma once

#include <array>
#include <functional>
#include <optional>

#include "lsm/db.hpp"
#include "merging_iterator.hpp"
#include "version_set.hpp"

namespace lsm::internal {
using CompactionPointers = std::array<std::string, 7>;

struct CompactionPlan {
  std::shared_ptr<const Version> base;
  std::uint32_t level = 0;
  std::vector<TableMetadata> inputs;
  std::string largest;
};

auto PickCompaction(std::shared_ptr<const Version> version, const Options& options,
                    const CompactionPointers& pointers) -> std::optional<CompactionPlan>;

// Emit retained entries as they are merged. The vector overload is a reference
// adapter for tests and leaves its output unchanged on failure.
auto CompactEntries(MergingIterator& input, const CompactionPlan& plan,
                    std::uint64_t oldest_snapshot,
                    const std::function<Status(const table::Entry&)>& emit) -> Status;
auto CompactEntries(MergingIterator& input, const CompactionPlan& plan,
                    std::uint64_t oldest_snapshot, std::vector<table::Entry>& output) -> Status;
}  // namespace lsm::internal
