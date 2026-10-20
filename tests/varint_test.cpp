#include "util/varint.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace lsm::internal {
namespace {

template <typename UInt>
void Encode(std::string& output, UInt value) {
  if constexpr (sizeof(UInt) == 4) {
    AppendVarint32(output, value);
  } else {
    AppendVarint64(output, value);
  }
}

template <typename UInt>
bool Decode(std::string_view& input, UInt& value) {
  if constexpr (sizeof(UInt) == 4) {
    return ConsumeVarint32(input, value);
  } else {
    return ConsumeVarint64(input, value);
  }
}

// Division/remainder reference rather than the production shift/mask loop.
template <typename UInt>
std::string ReferenceEncode(UInt value) {
  std::string output;
  do {
    const auto digit = value % 128;
    value /= 128;
    output.push_back(static_cast<char>(digit + (value != 0 ? 128 : 0)));
  } while (value != 0);
  return output;
}

// Reconstruct from the most significant group using checked multiplication.
template <typename UInt>
std::optional<std::pair<UInt, std::size_t>> ReferenceDecode(std::string_view input) {
  std::size_t terminator = 0;
  while (terminator < input.size() && static_cast<unsigned char>(input[terminator]) >= 128) {
    ++terminator;
  }
  if (terminator == input.size() || (terminator != 0 && input[terminator] == '\0')) {
    return std::nullopt;
  }
  UInt result = 0;
  for (auto count = terminator + 1; count > 0; --count) {
    const auto digit = static_cast<UInt>(static_cast<unsigned char>(input[count - 1]) % 128);
    if (result > (std::numeric_limits<UInt>::max() - digit) / 128) {
      return std::nullopt;
    }
    result = result * 128 + digit;
  }
  return std::pair{result, terminator + 1};
}

template <typename UInt>
class VarintTest : public ::testing::Test {};
using IntegerTypes = ::testing::Types<std::uint32_t, std::uint64_t>;
TYPED_TEST_SUITE(VarintTest, IntegerTypes);

TYPED_TEST(VarintTest, GoldenWireBytesAndAppendSemantics) {
  const std::array<TypeParam, 7> values{0, 1, 127, 128, 255, 300, 16384};
  const std::array<std::string, 7> expected{
      std::string(1, '\0'), "\x01", "\x7f", "\x80\x01", "\xff\x01", "\xac\x02", "\x80\x80\x01"};
  for (std::size_t i = 0; i < values.size(); ++i) {
    std::string encoded = "prefix";
    Encode(encoded, values[i]);
    EXPECT_EQ(encoded, "prefix" + expected[i]);
    std::string_view input(encoded);
    input.remove_prefix(6);
    TypeParam decoded = 99;
    ASSERT_TRUE(Decode(input, decoded));
    EXPECT_EQ(decoded, values[i]);
    EXPECT_TRUE(input.empty());
  }
}

TYPED_TEST(VarintTest, EveryBitBoundaryMaximumAndTruncatedPrefix) {
  std::vector<TypeParam> values{0, std::numeric_limits<TypeParam>::max()};
  for (int bit = 0; bit < std::numeric_limits<TypeParam>::digits; ++bit) {
    const auto boundary = static_cast<TypeParam>(TypeParam{1} << bit);
    values.push_back(boundary);
    values.push_back(boundary - 1);
    values.push_back(boundary + 1);
  }
  for (const auto value : values) {
    SCOPED_TRACE(value);
    std::string encoded;
    Encode(encoded, value);
    EXPECT_EQ(encoded, ReferenceEncode(value));
    for (std::size_t cut = 0; cut < encoded.size(); ++cut) {
      const std::string_view original(encoded.data(), cut);
      auto input = original;
      TypeParam decoded = 123;
      EXPECT_FALSE(Decode(input, decoded));
      EXPECT_EQ(input.data(), original.data());
      EXPECT_EQ(input, original);
      EXPECT_EQ(decoded, 123U);
    }
    encoded += "tail";
    std::string_view input(encoded);
    TypeParam decoded = 0;
    ASSERT_TRUE(Decode(input, decoded));
    EXPECT_EQ(decoded, value);
    EXPECT_EQ(input, "tail");
  }
}

TYPED_TEST(VarintTest, RejectsMalformedWithoutChangingArguments) {
  constexpr std::size_t kMaxBytes = (std::numeric_limits<TypeParam>::digits + 6) / 7;
  std::string overflow(kMaxBytes, static_cast<char>(0xff));
  overflow.back() = sizeof(TypeParam) == 4 ? '\x10' : '\x02';
  std::string overlong(kMaxBytes + 1, static_cast<char>(0x80));
  overlong.back() = '\x01';
  const std::vector<std::string> invalid{"",
                                         "\x80",
                                         std::string("\x80\x00", 2),
                                         std::string("\x81\x00", 2),
                                         std::string(kMaxBytes, static_cast<char>(0x80)),
                                         overflow,
                                         overlong};
  for (const auto& bytes : invalid) {
    std::string_view input(bytes);
    const auto original = input;
    TypeParam value = 123;
    EXPECT_FALSE(Decode(input, value));
    EXPECT_EQ(input.data(), original.data());
    EXPECT_EQ(input, original);
    EXPECT_EQ(value, 123U);
  }
}

TYPED_TEST(VarintTest, RandomizedRoundTripAndReferenceEncoding) {
  SCOPED_TRACE(::testing::Message() << "seed=" << test::seed);
  std::mt19937_64 random(test::seed);
  for (int iteration = 0; iteration < 100000; ++iteration) {
    SCOPED_TRACE(iteration);
    const auto value = static_cast<TypeParam>(random());
    std::string encoded;
    Encode(encoded, value);
    EXPECT_EQ(encoded, ReferenceEncode(value));
    std::string_view input(encoded);
    TypeParam decoded = 0;
    ASSERT_TRUE(Decode(input, decoded));
    EXPECT_EQ(decoded, value);
    EXPECT_TRUE(input.empty());
  }
}

TYPED_TEST(VarintTest, RandomizedMalformedInputMatchesCheckedReference) {
  SCOPED_TRACE(::testing::Message() << "seed=" << test::seed);
  std::mt19937_64 random(test::seed);
  for (int iteration = 0; iteration < 50000; ++iteration) {
    SCOPED_TRACE(iteration);
    std::string bytes(static_cast<std::size_t>(random() % 13), '\0');
    for (auto& byte : bytes) {
      byte = static_cast<char>(random() & 0xffU);
    }
    const auto expected = ReferenceDecode<TypeParam>(bytes);
    const std::string_view original(bytes);
    auto input = original;
    TypeParam value = 123;
    ASSERT_EQ(Decode(input, value), expected.has_value());
    if (expected) {
      EXPECT_EQ(value, expected->first);
      EXPECT_EQ(input, original.substr(expected->second));
    } else {
      EXPECT_EQ(input.data(), original.data());
      EXPECT_EQ(input, original);
      EXPECT_EQ(value, 123U);
    }
  }
}

TYPED_TEST(VarintTest, ConcatenatedStream) {
  const std::array<TypeParam, 5> values{0, 128, 127, std::numeric_limits<TypeParam>::max(), 1};
  std::string encoded;
  for (const auto value : values) {
    Encode(encoded, value);
  }
  std::string_view input(encoded);
  for (const auto expected : values) {
    TypeParam actual = 0;
    ASSERT_TRUE(Decode(input, actual));
    EXPECT_EQ(actual, expected);
  }
  EXPECT_TRUE(input.empty());
}

}  // namespace
}  // namespace lsm::internal
