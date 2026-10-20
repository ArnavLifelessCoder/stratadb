#include "iterator.hpp"

#include <gtest/gtest.h>

#include "table/format.hpp"

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
    auto iterator = MakeIterator(streams, sequence, {}, {}, {});
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
  auto malformed = MakeIterator({{Entry("z", 1, ""), Entry("a", 2, "")}}, 2, {}, {}, {});
  EXPECT_EQ(malformed->status().code(), StatusCode::kCorruption);
  malformed->SeekToFirst();
  EXPECT_FALSE(malformed->Valid());
  auto conflict = MakeIterator({{Entry("a", 1, "left")}, {Entry("a", 1, "right")}}, 1, {}, {}, {});
  EXPECT_EQ(conflict->status().code(), StatusCode::kCorruption);
  conflict->SeekToFirst();
  EXPECT_FALSE(conflict->Valid());
}
}  // namespace
}  // namespace lsm::internal
