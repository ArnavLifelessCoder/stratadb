#include "util/hash.hpp"

#include <gtest/gtest.h>

#include <string>

#include "hash_vectors.hpp"

namespace lsm::internal {
namespace {

TEST(HashTest, EmptyAndTextVectors) {
  EXPECT_EQ(Hash64({}), 0ULL);
  EXPECT_EQ(Hash64("foo"), 0xe271865701f54561ULL);
  EXPECT_EQ(Hash64("hello"), 0xcbd8a7b341bd9b02ULL);
}

TEST(HashTest, IndependentOracleCoversBlocksTailsSeedsAndAlignment) {
  for (const auto& vector : test::kHashVectors) {
    SCOPED_TRACE(::testing::Message() << "length=" << vector.length << " seed=" << vector.seed);
    std::string key(vector.length, '\0');
    for (std::size_t i = 0; i < key.size(); ++i) {
      key[i] = static_cast<char>((i * 37 + 11) % 256);
    }
    for (std::size_t offset = 0; offset < 8; ++offset) {
      SCOPED_TRACE(offset);
      const auto storage = std::string(offset, 'x') + key;
      EXPECT_EQ(Hash64(std::string_view(storage).substr(offset), vector.seed), vector.expected);
    }
  }
}

TEST(HashTest, LengthAndEmbeddedNulsAreSignificant) {
  EXPECT_NE(Hash64(std::string_view("a\0b", 3)), Hash64("a"));
  EXPECT_NE(Hash64(std::string_view("\0", 1)), Hash64({}));
  EXPECT_NE(Hash64("same", 0), Hash64("same", 1));
}

}  // namespace
}  // namespace lsm::internal
