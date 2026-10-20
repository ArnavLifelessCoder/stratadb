#pragma once

#include <functional>

#include "lsm/iterator.hpp"
#include "version_set.hpp"

namespace lsm::internal {
auto MakeIterator(std::vector<std::vector<table::Entry>> streams, std::uint64_t sequence,
                  std::shared_ptr<const Version> version, Status status,
                  std::function<void()> released) -> std::unique_ptr<Iterator>;
}  // namespace lsm::internal
