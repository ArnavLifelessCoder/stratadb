#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/status.hpp"

namespace lsm::internal::table {
struct Entry {
  std::string key;
  std::string value;
  auto operator==(const Entry&) const -> bool = default;
};

// Internal keys must be strictly ordered. Both functions preserve output on error.
// Encoding includes restart offsets, restart count and a trailing CRC32C.
auto EncodeBlock(std::span<const Entry> entries, std::string& output) -> Status;
auto DecodeBlock(std::string_view bytes, std::vector<Entry>& output) -> Status;
}  // namespace lsm::internal::table
