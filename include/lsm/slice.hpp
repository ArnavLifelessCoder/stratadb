#pragma once

#include <cstddef>
#include <string_view>

namespace lsm {

// A non-owning binary view. The caller keeps the underlying bytes alive.
using Slice = std::string_view;

// Compare unsigned bytes lexicographically, returning -1, 0, or 1.
[[nodiscard]] inline int CompareBytes(Slice left, Slice right) noexcept {
  const int order = left.compare(right);
  return (order > 0) - (order < 0);
}

[[nodiscard]] inline std::size_t CommonPrefixLength(Slice left, Slice right) noexcept {
  std::size_t length = 0;
  while (length < left.size() && length < right.size() && left[length] == right[length]) {
    ++length;
  }
  return length;
}

// Failed consumption leaves input unchanged.
[[nodiscard]] inline bool ConsumePrefix(Slice& input, Slice prefix) noexcept {
  if (!input.starts_with(prefix)) {
    return false;
  }
  input.remove_prefix(prefix.size());
  return true;
}

}  // namespace lsm
