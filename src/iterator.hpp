#pragma once

#include <functional>

#include "internal_iterator.hpp"
#include "lsm/iterator.hpp"
#include "version_set.hpp"

namespace lsm::internal {
auto MakeVersionIterators(const std::shared_ptr<const Version>& version)
    -> std::vector<std::unique_ptr<InternalIterator>>;
auto MakeIterator(std::vector<std::unique_ptr<InternalIterator>> children, std::uint64_t sequence,
                  std::shared_ptr<const Version> version, Status status,
                  std::function<void()> released) -> std::unique_ptr<Iterator>;
}  // namespace lsm::internal
