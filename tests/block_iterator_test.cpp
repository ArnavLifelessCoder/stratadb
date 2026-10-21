#include "table/block_iterator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "table/block.hpp"
#include "table/format.hpp"
#include "test_support.hpp"

namespace lsm::internal::table {
namespace {
TEST(BlockIteratorTest, NavigationAndRestartSeekMatchFullDecoder) {
  std::vector<Entry> entries;
  for (std::uint64_t i = 0; i < 1024; ++i)
    entries.push_back(
        {InternalKey(std::to_string(100000 + i / 4), 4 - i % 4, WriteBatch::Type::kPut),
         std::to_string(i)});
  std::string bytes;
  ASSERT_TRUE(EncodeBlock(entries, bytes).ok());
  std::vector<Entry> reference;
  ASSERT_TRUE(DecodeBlock(bytes, reference).ok());
  BlockIterator it(bytes);
  EXPECT_FALSE(it.Valid());
  it.SeekToFirst();
  for (const auto& entry : reference) {
    ASSERT_TRUE(it.Valid()) << it.status().ToString();
    EXPECT_EQ(it.key(), entry.key);
    EXPECT_EQ(it.value(), entry.value);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
  it.SeekToLast();
  for (auto entry = reference.rbegin(); entry != reference.rend(); ++entry) {
    ASSERT_TRUE(it.Valid()) << it.status().ToString();
    EXPECT_EQ(it.key(), entry->key);
    EXPECT_EQ(it.value(), entry->value);
    it.Prev();
  }
  EXPECT_FALSE(it.Valid());
  std::mt19937_64 random(test::seed);
  for (unsigned step = 0; step < 1000; ++step) {
    const auto target =
        InternalKey(std::to_string(99999 + random() % 260), random() % 6, WriteBatch::Type::kPut);
    const auto expected = std::ranges::lower_bound(
        reference, target, [](const auto& a, const auto& b) { return Compare(a, b) < 0; },
        &Entry::key);
    const auto before = it.DecodedEntries();
    it.Seek(target);
    EXPECT_LE(it.DecodedEntries() - before, 26U);
    ASSERT_EQ(it.Valid(), expected != reference.end());
    if (it.Valid()) {
      EXPECT_EQ(it.key(), expected->key);
      if (expected != reference.begin()) {
        it.Prev();
        EXPECT_EQ(it.key(), std::prev(expected)->key);
        it.Next();
        EXPECT_EQ(it.key(), expected->key);
      }
    }
  }
  EXPECT_TRUE(it.status().ok());
}

TEST(BlockIteratorTest, EmptyCorruptAndMalformedBlocksHaveStickyStatus) {
  std::string bytes;
  ASSERT_TRUE(EncodeBlock({}, bytes).ok());
  BlockIterator empty(bytes);
  empty.SeekToFirst();
  empty.SeekToLast();
  empty.Next();
  empty.Prev();
  EXPECT_FALSE(empty.Valid());
  EXPECT_TRUE(empty.status().ok());
  bytes[0] ^= 1;
  BlockIterator bad_crc(bytes);
  EXPECT_EQ(bad_crc.status().code(), StatusCode::kCorruption);
  bad_crc.SeekToFirst();
  EXPECT_FALSE(bad_crc.Valid());
  const std::vector<Entry> entries{{InternalKey("key", 1, WriteBatch::Type::kPut), "value"}};
  ASSERT_TRUE(EncodeBlock(entries, bytes).ok());
  bytes.resize(bytes.size() - 4);
  bytes[bytes.size() - 8] = '\xff';
  Seal(bytes);
  BlockIterator malformed(bytes);
  EXPECT_EQ(malformed.status().code(), StatusCode::kCorruption);
}
}  // namespace
}  // namespace lsm::internal::table
