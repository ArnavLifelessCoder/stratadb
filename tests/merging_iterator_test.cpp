#include "merging_iterator.hpp"

#include <gtest/gtest.h>

#include <random>

#include "test_support.hpp"
#include "vector_iterator.hpp"

namespace lsm::internal::streaming {
namespace {
TEST(MergingIteratorTest, RandomDirectionChangesAndSeeksMatchFullSort) {
  std::mt19937_64 random(test::seed);
  std::vector<std::vector<table::Entry>> streams(7);
  std::vector<table::Entry> entries;
  for (std::uint64_t i = 1; i <= 1000; ++i) {
    table::Entry entry{
        table::InternalKey(std::to_string(random() % 100), i, WriteBatch::Type::kPut),
        std::to_string(i)};
    streams[random() % streams.size()].push_back(entry);
    entries.push_back(entry);
  }
  const auto before = [](const auto& a, const auto& b) { return table::Compare(a.key, b.key) < 0; };
  for (auto& stream : streams) std::ranges::sort(stream, before);
  std::ranges::sort(entries, before);
  test::VectorIterator reference(entries);
  MergingIterator merged(test::MakeIterators(std::move(streams)));
  EXPECT_FALSE(merged.Valid());
  for (unsigned step = 0; step < 10000; ++step) {
    SCOPED_TRACE(step);
    switch (random() % 5) {
      case 0:
        merged.Next();
        reference.Next();
        break;
      case 1:
        merged.Prev();
        reference.Prev();
        break;
      case 2:
        merged.SeekToFirst();
        reference.SeekToFirst();
        break;
      case 3:
        merged.SeekToLast();
        reference.SeekToLast();
        break;
      default: {
        const auto target = table::InternalKey(std::to_string(random() % 110), random() % 1002,
                                               WriteBatch::Type::kPut);
        merged.Seek(target);
        reference.Seek(target);
        break;
      }
    }
    ASSERT_TRUE(merged.status().ok()) << merged.status().ToString();
    ASSERT_EQ(merged.Valid(), reference.Valid());
    EXPECT_EQ(merged.key(), reference.key());
    EXPECT_EQ(merged.value(), reference.value());
  }
}

TEST(MergingIteratorTest, DuplicateTieOrderSurvivesDirectionChanges) {
  const auto key = table::InternalKey("key", 1, WriteBatch::Type::kPut);
  MergingIterator it(test::MakeIterators({{{key, "left"}}, {{key, "right"}}}));
  it.SeekToFirst();
  EXPECT_EQ(it.value(), "left");
  it.Next();
  EXPECT_EQ(it.value(), "right");
  it.Prev();
  EXPECT_EQ(it.value(), "left");
  it.Next();
  EXPECT_EQ(it.value(), "right");
  it.SeekToLast();
  EXPECT_EQ(it.value(), "right");
  it.Prev();
  EXPECT_EQ(it.value(), "left");
  it.Prev();
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.status().ok());
}
}  // namespace
}  // namespace lsm::internal::streaming
