#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lsm::internal {

// Unsigned base-128 encoding, least significant group first.
void AppendVarint32(std::string& destination, std::uint32_t value);
void AppendVarint64(std::string& destination, std::uint64_t value);

// Read one canonical varint. Reject truncation, overflow and redundant groups.
// On failure, both input and value remain unchanged. Trailing bytes are allowed.
[[nodiscard]] auto ConsumeVarint32(std::string_view& input, std::uint32_t& value) noexcept -> bool;
[[nodiscard]] auto ConsumeVarint64(std::string_view& input, std::uint64_t& value) noexcept -> bool;

}  // namespace lsm::internal
