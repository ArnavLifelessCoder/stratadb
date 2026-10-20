#include "write_batch.hpp"

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "test_support.hpp"

namespace lsm::internal {
namespace {
TEST(WriteBatchTest, OwnsDataPreservesOrderAndRoundTrips) {
  WriteBatch batch;
  std::string key("a\0z", 3);
  ASSERT_TRUE(batch.Put(key, "").ok());
  ASSERT_TRUE(batch.Delete(key).ok());
  ASSERT_TRUE(batch.Put("", std::string(1024 * 1024, 'v')).ok());
  key.assign("changed");
  std::string encoded;
  ASSERT_TRUE(EncodeBatch(batch, 7, encoded).ok());
  WriteBatch decoded;
  std::uint64_t sequence = 0;
  ASSERT_TRUE(DecodeBatch(encoded, sequence, decoded).ok());
  EXPECT_EQ(sequence, 7U);
  ASSERT_EQ(decoded.Count(), 3U);
  for (std::size_t i = 0; i < batch.Count(); ++i) {
    EXPECT_EQ(decoded.Entries()[i].type, batch.Entries()[i].type);
    EXPECT_EQ(decoded.Entries()[i].key, batch.Entries()[i].key);
    EXPECT_EQ(decoded.Entries()[i].value, batch.Entries()[i].value);
  }
  batch.Clear();
  EXPECT_EQ(batch.Count(), 0U);
}

TEST(WriteBatchTest, RejectsTruncationInvalidTypesTrailingBytesAndSequenceOverflow) {
  WriteBatch batch;
  ASSERT_TRUE(batch.Put("key", "value").ok());
  std::string encoded;
  ASSERT_TRUE(EncodeBatch(batch, kMaxSequence, encoded).ok());
  WriteBatch output;
  ASSERT_TRUE(output.Put("sentinel", "unchanged").ok());
  std::uint64_t sequence = 42;
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    EXPECT_FALSE(DecodeBatch(std::string_view(encoded).substr(0, length), sequence, output).ok());
    EXPECT_EQ(sequence, 42U);
    EXPECT_EQ(output.Entries()[0].key, "sentinel");
  }
  EXPECT_FALSE(DecodeBatch(encoded + "x", sequence, output).ok());
  ASSERT_TRUE(EncodeBatch(batch, 1, encoded).ok());
  encoded[2] = 99;
  EXPECT_FALSE(DecodeBatch(encoded, sequence, output).ok());
  ASSERT_TRUE(batch.Delete("key").ok());
  const auto previous = encoded;
  EXPECT_FALSE(EncodeBatch(batch, kMaxSequence, encoded).ok());
  EXPECT_EQ(encoded, previous);
  EXPECT_FALSE(EncodeBatch(batch, 0, encoded).ok());
  EXPECT_FALSE(EncodeBatch(batch, kMaxSequence + 1, encoded).ok());
}

TEST(WriteBatchTest, RandomBytesNeverProduceUnstableDecodes) {
  SCOPED_TRACE(test::seed);
  std::mt19937_64 random(test::seed);
  for (int i = 0; i < 10000; ++i) {
    std::string bytes(static_cast<std::size_t>(random() % 128), '\0');
    for (auto& byte : bytes) {
      byte = static_cast<char>(random());
    }
    WriteBatch batch;
    std::uint64_t sequence = 0;
    if (DecodeBatch(bytes, sequence, batch).ok()) {
      std::string encoded;
      ASSERT_TRUE(EncodeBatch(batch, sequence, encoded).ok());
      EXPECT_EQ(bytes, encoded);
    }
  }
}
}  // namespace
}  // namespace lsm::internal
