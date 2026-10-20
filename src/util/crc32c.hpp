#pragma once

#include <cstdint>
#include <string_view>

namespace lsm::internal {

[[nodiscard]] auto HasSse42Crc32c() noexcept -> bool;
[[nodiscard]] auto ExtendCrc32cTable(std::uint32_t crc, std::string_view bytes) noexcept
    -> std::uint32_t;
// Call only when HasSse42Crc32c() returns true.
[[nodiscard]] auto ExtendCrc32cSse42(std::uint32_t crc, std::string_view bytes) noexcept
    -> std::uint32_t;

// Reflected Castagnoli CRC, initial/final XOR 0xffffffff. Empty input has CRC 0.
// The return value is numeric; file formats serialize it explicitly.
[[nodiscard]] auto ExtendCrc32c(std::uint32_t crc, std::string_view bytes) noexcept
    -> std::uint32_t;
[[nodiscard]] inline auto Crc32c(std::string_view bytes) noexcept -> std::uint32_t {
  return ExtendCrc32c(0, bytes);
}

}  // namespace lsm::internal
