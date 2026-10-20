#pragma once

#include <array>
#include <optional>

#include "lsm/db.hpp"
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

// K-way merge over sorted, CRC-checked table/memtable entry streams. Owned
// streams currently reside in memory; the heap has one cursor per stream.
class MergingIterator {
 public:
  explicit MergingIterator(std::vector<std::vector<table::Entry>> streams);
  [[nodiscard]] auto Valid() const -> bool { return status_.ok() && !heap_.empty(); }
  [[nodiscard]] auto entry() const -> const table::Entry&;
  void Next();
  [[nodiscard]] auto status() const -> Status { return status_; }

 private:
  auto Later(std::size_t left, std::size_t right) const -> bool;
  std::vector<std::vector<table::Entry>> streams_;
  std::vector<std::size_t> positions_;
  std::vector<std::size_t> heap_;
  Status status_;
};

auto CompactEntries(MergingIterator& input, const CompactionPlan& plan,
                    std::uint64_t oldest_snapshot, std::vector<table::Entry>& output) -> Status;
}  // namespace lsm::internal
