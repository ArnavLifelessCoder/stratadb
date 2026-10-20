#include "lsm/slice.hpp"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace lsm {
namespace {

static_assert(std::is_same_v<Slice, std::string_view>);

TEST(SliceTest, BinaryViewKeepsEmbeddedNulsAndDoesNotOwnBytes) {
  const std::string bytes("a\0b", 3);
  const Slice view(bytes);
  EXPECT_EQ(view.data(), bytes.data());
  EXPECT_EQ(view.size(), 3U);
  EXPECT_EQ(view[1], '\0');
  EXPECT_GT(CompareBytes(view, "a"), 0);
}

TEST(SliceTest, AllSingleBytesHaveUnsignedOrdering) {
  for (unsigned left = 0; left < 256; ++left) {
    for (unsigned right = 0; right < 256; ++right) {
      const char a = static_cast<char>(left);
      const char b = static_cast<char>(right);
      EXPECT_EQ(CompareBytes(Slice(&a, 1), Slice(&b, 1)), (left > right) - (left < right));
    }
  }
}

TEST(SliceTest, EmptyAndPrefixOrdering) {
  EXPECT_EQ(CompareBytes({}, {}), 0);
  EXPECT_EQ(CompareBytes({}, "a"), -1);
  EXPECT_EQ(CompareBytes("a", {}), 1);
  EXPECT_EQ(CompareBytes("a", "ab"), -1);
  EXPECT_EQ(CompareBytes("ab", "a"), 1);
  EXPECT_EQ(CompareBytes("abc", "abc"), 0);
  EXPECT_EQ(CommonPrefixLength({}, {}), 0U);
  EXPECT_EQ(CommonPrefixLength("abc", "abd"), 2U);
  EXPECT_EQ(CommonPrefixLength("ab", "abc"), 2U);
  EXPECT_EQ(CommonPrefixLength("abc", "ab"), 2U);
  EXPECT_EQ(CommonPrefixLength(Slice("a\0c", 3), Slice("a\0d", 3)), 2U);
}

TEST(SliceTest, ConsumePrefixIsTransactionalAndHandlesAliasing) {
  Slice input("a\0b", 3);
  const auto original = input;
  EXPECT_FALSE(ConsumePrefix(input, "ax"));
  EXPECT_EQ(input.data(), original.data());
  EXPECT_EQ(input, original);
  EXPECT_FALSE(ConsumePrefix(input, "longer"));
  EXPECT_TRUE(ConsumePrefix(input, {}));
  EXPECT_EQ(input, original);
  EXPECT_TRUE(ConsumePrefix(input, input.substr(0, 2)));
  EXPECT_EQ(input, "b");
  EXPECT_TRUE(ConsumePrefix(input, input));
  EXPECT_TRUE(input.empty());
  Slice empty;
  EXPECT_TRUE(ConsumePrefix(empty, {}));
  EXPECT_FALSE(ConsumePrefix(empty, "x"));
}

}  // namespace
}  // namespace lsm
