#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lsm/write_batch.hpp"

namespace lsm::internal {

inline constexpr std::uint64_t kMaxSequence = (std::uint64_t{1} << 56) - 1;
// Both codecs leave outputs untouched on failure. Empty batches are not WAL records.
auto EncodeBatch(const WriteBatch& batch, std::uint64_t sequence, std::string& output) -> Status;
auto DecodeBatch(std::string_view input, std::uint64_t& sequence, WriteBatch& batch) -> Status;

}  // namespace lsm::internal
