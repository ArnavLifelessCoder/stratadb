#include "table/block.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "table/format.hpp"
#include "test_support.hpp"

namespace lsm::internal::table {
namespace {
TEST(BlockTest, RoundTripMatchesUncompressedReferenceAtEveryRestartBoundary) {
  std::mt19937_64 random(test::seed);
  for (const std::size_t count : {0U, 1U, 15U, 16U, 17U, 31U, 32U, 33U, 1000U}) {
    SCOPED_TRACE(count);
    std::vector<Entry> reference;
    for (std::size_t i = 0; i < count; ++i) {
      auto user = std::string(i % 137, static_cast<char>(random()));
      user.append(std::to_string(i));
      reference.push_back({.key = InternalKey(user, i + 1, WriteBatch::Type::kPut),
                           .value = std::string(i % 101, static_cast<char>(random()))});
    }
    std::ranges::sort(reference,
                      [](const Entry& a, const Entry& b) { return Compare(a.key, b.key) < 0; });
    std::string encoded;
    ASSERT_TRUE(EncodeBlock(reference, encoded).ok());
    std::vector<Entry> decoded;
    ASSERT_TRUE(DecodeBlock(encoded, decoded).ok());
    EXPECT_EQ(decoded, reference);
  }
}

TEST(BlockTest, NumericDescendingSequenceOrderEmptyKeysAndLargeValues) {
  const std::vector<Entry> entries{
      {.key = InternalKey("", kMaxSequence, WriteBatch::Type::kPut), .value = ""},
      {.key = InternalKey("", 256, WriteBatch::Type::kPut), .value = std::string(1 << 20, '\0')},
      {.key = InternalKey("", 255, WriteBatch::Type::kDelete), .value = ""},
      {.key = InternalKey(std::string("\0\xff", 2), 1, WriteBatch::Type::kPut), .value = "binary"}};
  std::string bytes;
  ASSERT_TRUE(EncodeBlock(entries, bytes).ok());
  std::vector<Entry> decoded;
  ASSERT_TRUE(DecodeBlock(bytes, decoded).ok());
  EXPECT_EQ(decoded, entries);
}

TEST(BlockTest, EveryFlippedByteAndTruncationIsRejectedWithoutChangingOutput) {
  const std::vector<Entry> entries{
      {.key = InternalKey("prefix-a", 9, WriteBatch::Type::kPut), .value = "value"},
      {.key = InternalKey("prefix-b", 8, WriteBatch::Type::kPut), .value = ""}};
  std::string bytes;
  ASSERT_TRUE(EncodeBlock(entries, bytes).ok());
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    auto damaged = bytes;
    damaged[i] ^= 1;
    auto output = entries;
    EXPECT_EQ(DecodeBlock(damaged, output).code(), StatusCode::kCorruption);
    EXPECT_EQ(output, entries);
    EXPECT_EQ(DecodeBlock(std::string_view(bytes).substr(0, i), output).code(),
              StatusCode::kCorruption);
  }
}

TEST(BlockTest, RejectsMalformedStructureEvenWithValidCrc) {
  const std::vector<Entry> entries{
      {.key = InternalKey("a", 1, WriteBatch::Type::kPut), .value = "b"}};
  std::string bytes;
  ASSERT_TRUE(EncodeBlock(entries, bytes).ok());
  // Shared prefix, key length, value length, restart offset, restart count.
  for (const auto offset :
       {std::size_t{0}, std::size_t{1}, std::size_t{2}, bytes.size() - 12, bytes.size() - 8}) {
    auto damaged = bytes.substr(0, bytes.size() - 4);
    damaged[offset] = static_cast<char>(127);
    Seal(damaged);
    std::vector<Entry> output;
    EXPECT_EQ(DecodeBlock(damaged, output).code(), StatusCode::kCorruption);
  }
  auto invalid = entries;
  invalid.push_back(entries.front());
  std::string output = "sentinel";
  EXPECT_EQ(EncodeBlock(invalid, output).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(output, "sentinel");
}
}  // namespace
}  // namespace lsm::internal::table
