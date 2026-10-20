#include "util/crc32c.hpp"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string>

#include "test_support.hpp"

namespace lsm::internal {
namespace {

// Bit-at-a-time reference, deliberately independent of the production lookup table.
std::uint32_t ReferenceCrc32c(std::string_view bytes, std::uint32_t initial = 0) {
  auto crc = ~initial;
  for (const char byte : bytes) {
    const auto value = static_cast<unsigned char>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      const bool carry = ((crc & 1U) ^ ((value >> bit) & 1U)) != 0;
      crc >>= 1;
      if (carry) {
        crc ^= 0x82f63b78U;
      }
    }
  }
  return ~crc;
}

TEST(Crc32cTest, Rfc3720AppendixB4Vectors) {
  // RFC 3720 B.4 prints CRC octets in wire order, least significant octet first.
  // https://www.rfc-editor.org/rfc/rfc3720#appendix-B.4
  EXPECT_EQ(Crc32c(std::string(32, '\0')), 0x8a9136aaU);
  EXPECT_EQ(Crc32c(std::string(32, static_cast<char>(0xff))), 0x62a8ab43U);
  std::string ascending;
  std::string descending;
  for (int i = 0; i < 32; ++i) {
    ascending.push_back(static_cast<char>(i));
    descending.push_back(static_cast<char>(31 - i));
  }
  EXPECT_EQ(Crc32c(ascending), 0x46dd794eU);
  EXPECT_EQ(Crc32c(descending), 0x113fdb5cU);
  const std::array<unsigned char, 48> command{
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const std::string bytes(command.begin(), command.end());
  EXPECT_EQ(Crc32c(bytes), 0xd9963a56U);
}

TEST(Crc32cTest, EmptyAndCanonicalCheckValue) {
  EXPECT_EQ(Crc32c({}), 0U);
  EXPECT_EQ(Crc32c("123456789"), 0xe3069283U);
  EXPECT_EQ(ExtendCrc32c(0x12345678U, {}), 0x12345678U);
}

TEST(Crc32cTest, EverySplitAndSingleBitChange) {
  const std::string bytes("abc\0def\xff", 8);
  const auto expected = Crc32c(bytes);
  const std::string_view view(bytes);
  for (std::size_t split = 0; split <= view.size(); ++split) {
    EXPECT_EQ(ExtendCrc32c(Crc32c(view.substr(0, split)), view.substr(split)), expected);
  }
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      auto changed = bytes;
      changed[i] = static_cast<char>(static_cast<unsigned char>(changed[i]) ^ (1U << bit));
      EXPECT_NE(Crc32c(changed), expected);
    }
  }
}

TEST(Crc32cTest, RandomizedDifferentialAgainstBitwiseReference) {
  SCOPED_TRACE(::testing::Message() << "seed=" << test::seed);
  std::mt19937_64 random(test::seed);
  for (int iteration = 0; iteration < 1024; ++iteration) {
    SCOPED_TRACE(iteration);
    std::string storage(static_cast<std::size_t>(random() % 4097) + 1, '\0');
    for (auto& byte : storage) {
      byte = static_cast<char>(random() & 0xffU);
    }
    // Exercise unaligned buffers too.
    const std::string_view bytes(storage.data() + 1, storage.size() - 1);
    const auto initial = static_cast<std::uint32_t>(random());
    EXPECT_EQ(Crc32c(bytes), ReferenceCrc32c(bytes));
    EXPECT_EQ(ExtendCrc32c(initial, bytes), ReferenceCrc32c(bytes, initial));
  }
}

}  // namespace
}  // namespace lsm::internal
