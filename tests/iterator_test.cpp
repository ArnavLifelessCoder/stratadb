#include "iterator.hpp"

#include <gtest/gtest.h>

#include <map>
#include <random>

#include "table/format.hpp"
#include "test_support.hpp"
#include "vector_iterator.hpp"

namespace lsm::internal {
namespace {
auto Entry(std::string_view key, std::uint64_t sequence, std::string_view value,
           WriteBatch::Type type = WriteBatch::Type::kPut) -> table::Entry {
  return {.key = table::InternalKey(key, sequence, type), .value = std::string(value)};
}

TEST(IteratorTest, MergeSnapshotSeekForwardReverseAndBinaryKeys) {
  const std::string binary("b\0", 2);
  const std::vector<std::vector<table::Entry>> streams{
      {Entry("", 1, "empty"), Entry("a", 7, "new"), Entry(binary, 5, "binary")},
      {Entry("a", 3, "old"), Entry("c", 6, "", WriteBatch::Type::kDelete), Entry("c", 2, "old-c")}};
  for (const auto sequence : {4U, 7U}) {
    auto iterator = MakeIterator(test::MakeIterators(streams), sequence, {}, {}, {});
    EXPECT_FALSE(iterator->Valid());
    ASSERT_TRUE(iterator->status().ok());
    iterator->SeekToFirst();
    ASSERT_TRUE(iterator->Valid());
    EXPECT_EQ(iterator->key(), "");
    EXPECT_EQ(iterator->value(), "empty");
    iterator->Next();
    EXPECT_EQ(iterator->key(), "a");
    EXPECT_EQ(iterator->value(), sequence == 4 ? "old" : "new");
    iterator->Next();
    EXPECT_EQ(iterator->key(), sequence == 4 ? std::string_view("c") : std::string_view(binary));
    iterator->Next();
    EXPECT_FALSE(iterator->Valid());
    iterator->Next();
    EXPECT_FALSE(iterator->Valid());
    iterator->SeekToLast();
    ASSERT_TRUE(iterator->Valid());
    iterator->Prev();
    EXPECT_EQ(iterator->key(), "a");
    iterator->Prev();
    EXPECT_EQ(iterator->key(), "");
    iterator->Prev();
    EXPECT_FALSE(iterator->Valid());
    iterator->Seek("aa");
    ASSERT_TRUE(iterator->Valid());
    EXPECT_EQ(iterator->key(), sequence == 4 ? std::string_view("c") : std::string_view(binary));
    iterator->Seek("z");
    EXPECT_FALSE(iterator->Valid());
    EXPECT_TRUE(iterator->key().empty());
    EXPECT_TRUE(iterator->value().empty());
    EXPECT_TRUE(iterator->status().ok());
  }
}

TEST(IteratorTest, EmptyErrorsAndVersionLifetime) {
  auto version = std::make_shared<Version>();
  const std::weak_ptr<const Version> weak = version;
  bool released = false;
  auto iterator = MakeIterator({}, 1, version, {}, [&] { released = weak.expired(); });
  version.reset();
  EXPECT_FALSE(weak.expired());
  iterator->SeekToFirst();
  EXPECT_FALSE(iterator->Valid());
  iterator->SeekToLast();
  EXPECT_FALSE(iterator->Valid());
  iterator->Seek("key");
  EXPECT_FALSE(iterator->Valid());
  EXPECT_TRUE(iterator->status().ok());
  iterator.reset();
  EXPECT_TRUE(released);
  auto failed = MakeIterator({}, 1, {}, Status::IOError("injected table read"), {});
  EXPECT_EQ(failed->status().code(), StatusCode::kIOError);
  auto malformed =
      MakeIterator(test::MakeIterators({{Entry("z", 1, ""), Entry("a", 2, "")}}), 2, {}, {}, {});
  EXPECT_EQ(malformed->status().code(), StatusCode::kCorruption);
  malformed->SeekToFirst();
  EXPECT_FALSE(malformed->Valid());
  auto conflict = MakeIterator(
      test::MakeIterators({{Entry("a", 1, "left")}, {Entry("a", 1, "right")}}), 1, {}, {}, {});
  EXPECT_TRUE(conflict->status().ok());
  conflict->SeekToFirst();
  EXPECT_EQ(conflict->status().code(), StatusCode::kCorruption);
  EXPECT_FALSE(conflict->Valid());
}

TEST(IteratorTest, SnapshotFilteringAndDirectionChangesMatchOrderedMap) {
  std::mt19937_64 random(test::seed);
  std::vector<std::vector<table::Entry>> streams(5);
  std::vector<table::Entry> history;
  for (std::uint64_t sequence = 1; sequence <= 200; ++sequence) {
    const auto id = random() % 20;
    const auto key =
        id == 0 ? std::string{} : (id == 1 ? std::string("a\0b", 3) : std::to_string(id));
    const bool erased = random() % 3 == 0;
    const auto entry = Entry(key, sequence, erased ? "" : std::to_string(sequence),
                             erased ? WriteBatch::Type::kDelete : WriteBatch::Type::kPut);
    streams[random() % streams.size()].push_back(entry);
    history.push_back(entry);
  }
  for (auto& stream : streams)
    std::ranges::sort(
        stream, [](const auto& a, const auto& b) { return table::Compare(a.key, b.key) < 0; });
  for (const auto sequence : {0U, 1U, 50U, 100U, 200U}) {
    std::map<std::string, std::string> map;
    for (const auto& entry : history) {
      if ((table::Tag(entry.key) >> 8) > sequence) break;
      const std::string key(table::UserKey(entry.key));
      if ((table::Tag(entry.key) & 0xffU) == 0)
        map.erase(key);
      else
        map[key] = entry.value;
    }
    const std::vector<std::pair<std::string, std::string>> reference(map.begin(), map.end());
    auto it = MakeIterator(test::MakeIterators(streams), sequence, {}, {}, {});
    std::size_t position = reference.size();
    for (unsigned step = 0; step < 2000; ++step) {
      SCOPED_TRACE(sequence);
      SCOPED_TRACE(step);
      switch (random() % 5) {
        case 0:
          it->SeekToFirst();
          position = 0;
          break;
        case 1:
          it->SeekToLast();
          position = reference.empty() ? 0 : reference.size() - 1;
          break;
        case 2:
          it->Next();
          if (position < reference.size()) ++position;
          break;
        case 3:
          it->Prev();
          if (position < reference.size())
            position = position == 0 ? reference.size() : position - 1;
          break;
        default: {
          const auto key = std::to_string(random() % 22);
          it->Seek(key);
          position = static_cast<std::size_t>(
              std::ranges::lower_bound(reference, key, {},
                                       &std::pair<std::string, std::string>::first) -
              reference.begin());
          break;
        }
      }
      ASSERT_TRUE(it->status().ok()) << it->status().ToString();
      ASSERT_EQ(it->Valid(), position < reference.size());
      if (it->Valid()) {
        EXPECT_EQ(it->key(), reference[position].first);
        EXPECT_EQ(it->value(), reference[position].second);
      }
    }
  }
}
}  // namespace
}  // namespace lsm::internal
