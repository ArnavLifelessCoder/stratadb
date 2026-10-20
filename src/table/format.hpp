#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "util/crc32c.hpp"
#include "write_batch.hpp"

namespace lsm::internal::table {

template <unsigned Bytes>
inline void AppendFixed(std::string& output, std::uint64_t value) {
  static_assert(Bytes == 4 || Bytes == 8);
  for (unsigned i = 0; i < Bytes; ++i) {
    output.push_back(static_cast<char>((value >> (8 * i)) & 0xffU));
  }
}

// Callers validate the byte range before loading.
inline auto LoadFixed(std::string_view input, unsigned bytes) -> std::uint64_t {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < bytes; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(input[i])) << (8 * i);
  }
  return value;
}

inline void Seal(std::string& bytes) { AppendFixed<4>(bytes, Crc32c(bytes)); }

inline auto ChecksumValid(std::string_view bytes) -> bool {
  return bytes.size() >= 4 &&
         LoadFixed(bytes.substr(bytes.size() - 4), 4) == Crc32c(bytes.substr(0, bytes.size() - 4));
}

inline auto InternalKey(std::string_view key, std::uint64_t sequence, WriteBatch::Type type)
    -> std::string {
  std::string result(key);
  AppendFixed<8>(result, (sequence << 8) | static_cast<std::uint8_t>(type));
  return result;
}

inline auto UserKey(std::string_view key) -> std::string_view {
  return key.substr(0, key.size() - 8);
}

inline auto Tag(std::string_view key) -> std::uint64_t {
  return LoadFixed(key.substr(key.size() - 8), 8);
}

inline auto ValidKey(std::string_view key) -> bool {
  if (key.size() < 8) {
    return false;
  }
  const auto tag = Tag(key);
  return (tag >> 8) != 0 && (tag & 0xffU) <= 1;
}

// Inputs must be valid internal keys. Tags are numeric, descending, not bytewise.
inline auto Compare(std::string_view left, std::string_view right) -> int {
  const auto user = UserKey(left).compare(UserKey(right));
  if (user != 0) {
    return user;
  }
  return Tag(left) > Tag(right) ? -1 : (Tag(left) < Tag(right) ? 1 : 0);
}

}  // namespace lsm::internal::table
