#pragma once

#include "version_set.hpp"

namespace lsm::internal {
// Cleanup failures leave harmless unreferenced files for a later retry.
auto RemoveObsolete(const std::filesystem::path& path) -> bool;
// Only call during Open after successful recovery and before starting workers.
void CleanupUnreferenced(const std::filesystem::path& directory, const ManifestState& state);
}  // namespace lsm::internal
