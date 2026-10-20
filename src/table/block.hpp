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
// Lower bound in internal-key order. Checks the block CRC, binary-searches full
// restart keys, then decodes at most one group and its successor. The optional
// counter includes restart probes and supports complexity assertions in tests.
auto SeekBlock(std::string_view bytes, std::string_view target, Entry& output,
               std::size_t* decoded_entries = nullptr) -> Status;
}  // namespace lsm::internal::table
