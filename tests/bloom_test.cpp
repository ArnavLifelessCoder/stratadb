#include "table/bloom.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <random>

#include "table/format.hpp"
#include "test_support.hpp"
#include "util/hash.hpp"

namespace lsm::internal::table {
namespace {
TEST(BloomTest, MatchesIndependentBitVectorAndHasNoFalseNegatives) {
  const std::vector<std::string> keys{"", std::string("a\0b", 3), "\xff", "prefix", "prefix-long"};
  for (const int bits : {1, 3, 10, 30}) {
    std::string encoded;
    ASSERT_TRUE(Bloom::Build(keys, bits, encoded).ok());
    Bloom bloom;
    ASSERT_TRUE(Bloom::Open(encoded, bloom).ok());
    const auto count = (encoded.size() - 5) * 8;
    const auto probes = static_cast<unsigned char>(encoded[0]);
    std::vector<bool> reference(count, false);
    for (const auto& key : keys) {
      const auto hash = Hash64(key);
      for (unsigned i = 0; i < probes; ++i) {
        const auto position = ((hash & 0xffffffffU) + i * ((hash >> 32) | 1U)) % count;
        reference[static_cast<std::size_t>(position)] = true;
      }
      EXPECT_TRUE(bloom.MayContain(key));
    }
    for (std::size_t bit = 0; bit < count; ++bit) {
      EXPECT_EQ((static_cast<unsigned char>(encoded[1 + bit / 8]) & (1U << (bit % 8))) != 0,
                reference[bit]);
    }
  }
}

TEST(BloomTest, EmptyDisabledAndCorruptedFilters) {
  std::string encoded;
  ASSERT_TRUE(Bloom::Build({}, 10, encoded).ok());
  Bloom bloom;
  ASSERT_TRUE(Bloom::Open(encoded, bloom).ok());
  EXPECT_FALSE(bloom.MayContain(""));
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    auto damaged = encoded;
    damaged[i] ^= 1;
    EXPECT_EQ(Bloom::Open(damaged, bloom).code(), StatusCode::kCorruption);
    EXPECT_FALSE(bloom.MayContain(""));
  }
  auto malformed = encoded.substr(0, encoded.size() - 4);
  malformed[0] = 127;
  Seal(malformed);
  EXPECT_EQ(Bloom::Open(malformed, bloom).code(), StatusCode::kCorruption);
  ASSERT_TRUE(Bloom::Build({}, 0, encoded).ok());
  ASSERT_TRUE(Bloom::Open(encoded, bloom).ok());
  EXPECT_TRUE(bloom.MayContain("anything"));
  EXPECT_EQ(Bloom::Build({}, -1, encoded).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Bloom::Build({}, 31, encoded).code(), StatusCode::kInvalidArgument);
}

TEST(BloomTest, MeasuredFalsePositiveRate) {
  constexpr std::size_t kKeys = 10000;
  constexpr std::size_t kQueries = 200000;
  std::mt19937_64 random(test::seed);
  std::vector<std::string> keys;
  for (std::size_t i = 0; i < kKeys; ++i) {
    keys.push_back("present/" + std::to_string(i) + "/" + std::to_string(random()));
  }
  std::string encoded;
  ASSERT_TRUE(Bloom::Build(keys, 10, encoded).ok());
  Bloom bloom;
  ASSERT_TRUE(Bloom::Open(encoded, bloom).ok());
  for (const auto& key : keys) {
    ASSERT_TRUE(bloom.MayContain(key));
  }
  std::size_t positives = 0;
  for (std::size_t i = 0; i < kQueries; ++i) {
    positives += bloom.MayContain("absent/" + std::to_string(i) + "/" + std::to_string(random()));
  }
  const auto actual = static_cast<double>(positives) / kQueries;
  const auto theoretical = std::pow(1.0 - std::exp(-7.0 / 10.0), 7);
  std::cout << "BLOOM_RESULT {\"seed\":" << test::seed << ",\"keys\":" << kKeys
            << ",\"queries\":" << kQueries
            << ",\"bits_per_key\":10,\"probes\":7,\"positives\":" << positives
            << ",\"actual\":" << actual << ",\"theoretical\":" << theoretical << "}\n";
  EXPECT_GE(actual, theoretical / 2);
  EXPECT_LE(actual, theoretical * 2);
}
}  // namespace
}  // namespace lsm::internal::table
