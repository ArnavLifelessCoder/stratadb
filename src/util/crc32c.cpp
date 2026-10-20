#include "util/crc32c.hpp"

#include <array>

namespace lsm::internal {
namespace {

constexpr auto MakeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    auto remainder = i;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder >> 1) ^ ((remainder & 1U) != 0 ? 0x82f63b78U : 0U);
    }
    table[i] = remainder;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

std::uint32_t ExtendCrc32c(std::uint32_t crc, std::string_view bytes) noexcept {
  auto remainder = ~crc;
  for (const char byte : bytes) {
    remainder = kTable[(remainder ^ static_cast<unsigned char>(byte)) & 0xffU] ^ (remainder >> 8);
  }
  return ~remainder;
}

}  // namespace lsm::internal
