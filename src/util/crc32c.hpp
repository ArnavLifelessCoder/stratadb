#pragma once

#include <cstdint>
#include <string_view>

namespace lsm::internal {

// Reflected Castagnoli CRC, initial/final XOR 0xffffffff. Empty input has CRC 0.
// The return value is numeric; file formats serialize it explicitly.
[[nodiscard]] std::uint32_t ExtendCrc32c(std::uint32_t crc, std::string_view bytes) noexcept;
[[nodiscard]] inline std::uint32_t Crc32c(std::string_view bytes) noexcept {
  return ExtendCrc32c(0, bytes);
}

}  // namespace lsm::internal
