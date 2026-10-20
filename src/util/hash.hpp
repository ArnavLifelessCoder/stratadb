#pragma once

#include <cstdint>
#include <string_view>

namespace lsm::internal {

// Low 64 bits of MurmurHash3 x64_128, with explicit little-endian byte loads.
// Non-cryptographic: suitable for bloom filters, not integrity checks.
[[nodiscard]] std::uint64_t Hash64(std::string_view bytes, std::uint32_t seed = 0) noexcept;

}  // namespace lsm::internal
