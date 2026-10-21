#pragma once

#include "lsm/iterator.hpp"

namespace lsm::internal {
// Same navigation contract as the public iterator, but keys include their tag
// and use internal-key ordering. Seek accepts sequence-zero boundary keys.
class InternalIterator : public Iterator {};
}  // namespace lsm::internal
