#pragma once

#include "lsm/iterator.hpp"

namespace lsm::internal {
// Same navigation contract as the public iterator, but keys include their tag
// and use internal-key ordering. Seek accepts sequence-zero boundary keys.
class InternalIterator : public Iterator {
 public:
  // Status without a copy, for per-entry checks in scan loops.
  [[nodiscard]] virtual auto StatusRef() const -> const Status& = 0;
  [[nodiscard]] auto status() const -> Status final { return StatusRef(); }
};
}  // namespace lsm::internal
