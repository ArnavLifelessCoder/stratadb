#include "compaction.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>

#include "table/format.hpp"
#include "test_support.hpp"

namespace lsm::internal {
namespace {
auto File(std::uint64_t number, std::uint32_t level, std::uint64_t size, std::string_view smallest,
          std::string_view largest) -> TableMetadata {
  return {.number = number,
          .level = level,
          .size = size,
          .smallest = std::string(smallest),
          .largest = std::string(largest),
          .reader = {}};
}

auto Entry(std::string_view key, std::uint64_t sequence, bool deleted = false) -> table::Entry {
  return {.key = table::InternalKey(key, sequence,
                                    deleted ? WriteBatch::Type::kDelete : WriteBatch::Type::kPut),
          .value = deleted ? "" : std::to_string(sequence)};
}

void Sort(std::vector<table::Entry>& entries) {
  std::ranges::sort(entries, [](const auto& left, const auto& right) {
    return table::Compare(left.key, right.key) < 0;
  });
}

auto Visible(const std::vector<table::Entry>& entries, std::uint64_t sequence)
    -> std::map<std::string, std::string> {
  std::map<std::string, std::pair<std::uint64_t, std::optional<std::string>>> versions;
  for (const auto& entry : entries) {
    const auto current = table::Tag(entry.key) >> 8;
    const std::string key(table::UserKey(entry.key));
    if (current <= sequence && (!versions.contains(key) || current > versions.at(key).first)) {
      versions[key] = {current, (table::Tag(entry.key) & 0xffU) == 0
                                    ? std::nullopt
                                    : std::optional<std::string>(entry.value)};
    }
  }
  std::map<std::string, std::string> result;
  for (const auto& [key, version] : versions) {
    if (version.second) result[key] = *version.second;
  }
  return result;
}

TEST(CompactionTest, L0SelectsAllFilesAndOverlappingL1Only) {
  auto version = std::make_shared<Version>();
  version->files = {File(3, 0, 100, "c", "g"), File(4, 0, 100, "e", "m"), File(5, 0, 100, "f", "f"),
                    File(6, 0, 100, "l", "p"), File(7, 1, 100, "a", "b"), File(8, 1, 100, "d", "h"),
                    File(9, 1, 100, "i", "z"), File(10, 2, 100, "a", "z")};
  const auto plan = PickCompaction(version, {}, {});
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->level, 0U);
  std::vector<std::uint64_t> ids;
  for (const auto& file : plan->inputs) ids.push_back(file.number);
  EXPECT_EQ(ids, (std::vector<std::uint64_t>{3, 4, 5, 6, 8, 9}));
}

TEST(CompactionTest, HigherLevelsChooseHighestScoreAndRoundRobinKey) {
  auto version = std::make_shared<Version>();
  constexpr std::uint64_t mib = 1024 * 1024;
  version->files = {File(3, 1, 20 * mib, "a", "z"), File(4, 2, 150 * mib, "a", "f"),
                    File(5, 2, 150 * mib, "m", "z"), File(6, 3, 1, "b", "e")};
  CompactionPointers pointers;
  auto plan = PickCompaction(version, {}, pointers);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->level, 2U);
  EXPECT_EQ(plan->inputs.front().number, 4U);
  ASSERT_EQ(plan->inputs.size(), 2U);
  pointers[2] = plan->largest;
  plan = PickCompaction(version, {}, pointers);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->inputs.front().number, 5U);
  pointers[2] = plan->largest;
  plan = PickCompaction(version, {}, pointers);
  EXPECT_EQ(plan->inputs.front().number, 4U);
  version->files = {File(7, 6, 1000 * mib, "a", "z")};
  EXPECT_FALSE(PickCompaction(version, {}, {}));
}

TEST(CompactionTest, HeapMergeMatchesFullSortAndRejectsInvalidStreams) {
  std::mt19937_64 random(test::seed);
  std::vector<std::vector<table::Entry>> streams(7);
  std::vector<table::Entry> reference;
  for (std::uint64_t sequence = 1; sequence <= 1000; ++sequence) {
    const auto entry = Entry(std::to_string(random() % 100), sequence);
    streams[random() % streams.size()].push_back(entry);
    reference.push_back(entry);
  }
  for (auto& stream : streams) Sort(stream);
  Sort(reference);
  MergingIterator merged(std::move(streams));
  ASSERT_TRUE(merged.status().ok());
  for (const auto& entry : reference) {
    ASSERT_TRUE(merged.Valid());
    EXPECT_EQ(merged.entry().key, entry.key);
    EXPECT_EQ(merged.entry().value, entry.value);
    merged.Next();
  }
  EXPECT_FALSE(merged.Valid());
  merged.Next();
  MergingIterator invalid({{Entry("z", 1), Entry("a", 2)}});
  EXPECT_EQ(invalid.status().code(), StatusCode::kCorruption);
  EXPECT_FALSE(invalid.Valid());
}

TEST(CompactionTest, SnapshotBoundaryAndDeeperTombstonesPreventResurrection) {
  auto version = std::make_shared<Version>();
  version->files = {File(3, 2, 100, "retain", "retain")};
  CompactionPlan plan{.base = version, .level = 0, .inputs = {}, .largest = {}};
  std::vector<table::Entry> entries{Entry("drop", 5, true),     Entry("drop", 1),
                                    Entry("retain", 5, true),   Entry("versions", 9),
                                    Entry("versions", 7, true), Entry("versions", 5),
                                    Entry("versions", 3),       Entry("versions", 1)};
  Sort(entries);
  MergingIterator input({entries});
  std::vector<table::Entry> output;
  ASSERT_TRUE(CompactEntries(input, plan, 3, output).ok());
  EXPECT_EQ(output.size(), 7U);
  MergingIterator later({entries});
  ASSERT_TRUE(CompactEntries(later, plan, 8, output).ok());
  ASSERT_EQ(output.size(), 2U);
  EXPECT_EQ(table::UserKey(output[0].key), "retain");
  EXPECT_EQ(table::UserKey(output[1].key), "versions");
  EXPECT_EQ(table::Tag(output[1].key) >> 8, 9U);
  plan.level = 5;
  MergingIterator bottom({entries});
  ASSERT_TRUE(CompactEntries(bottom, plan, kMaxSequence, output).ok());
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(table::UserKey(output.front().key), "versions");
}

TEST(CompactionTest, RandomMergesPreserveEverySupportedSnapshotAgainstMapReference) {
  std::mt19937_64 random(test::seed);
  for (unsigned trial = 0; trial < 100; ++trial) {
    SCOPED_TRACE(trial);
    auto version = std::make_shared<Version>();
    version->files = {File(3, 2, 100, "0", "9")};
    CompactionPlan plan{.base = version, .level = 0, .inputs = {}, .largest = {}};
    std::vector<table::Entry> deeper;
    for (std::uint64_t key = 0; key < 20; ++key) {
      deeper.push_back(Entry(std::to_string(key), key + 1));
    }
    std::vector<std::vector<table::Entry>> streams(5);
    auto reference = deeper;
    for (std::uint64_t sequence = 21; sequence <= 120; ++sequence) {
      const auto entry = Entry(std::to_string(random() % 20), sequence, random() % 3 == 0);
      streams[random() % streams.size()].push_back(entry);
      reference.push_back(entry);
    }
    for (auto& stream : streams) Sort(stream);
    const auto oldest = random() % 121;
    MergingIterator input(std::move(streams));
    std::vector<table::Entry> compacted;
    ASSERT_TRUE(CompactEntries(input, plan, oldest, compacted).ok());
    compacted.insert(compacted.end(), deeper.begin(), deeper.end());
    for (auto sequence = oldest; sequence <= 120; ++sequence) {
      EXPECT_EQ(Visible(compacted, sequence), Visible(reference, sequence)) << sequence;
    }
  }
}

TEST(CompactionTest, ConflictingDuplicatesReturnCorruptionWithoutPublishingOutput) {
  auto version = std::make_shared<Version>();
  CompactionPlan plan{.base = version, .level = 0, .inputs = {}, .largest = {}};
  const auto entry = Entry("key", 1);
  auto different = entry;
  different.value = "different";
  MergingIterator input({{entry}, {different}});
  std::vector<table::Entry> output{Entry("unchanged", 2)};
  EXPECT_EQ(CompactEntries(input, plan, 1, output).code(), StatusCode::kCorruption);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(table::UserKey(output.front().key), "unchanged");
}
}  // namespace
}  // namespace lsm::internal
