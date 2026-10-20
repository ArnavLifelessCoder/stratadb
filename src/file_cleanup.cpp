#include "file_cleanup.hpp"

#include <charconv>
#include <iostream>
#include <set>

namespace lsm::internal {
namespace {
auto Numbered(const std::filesystem::path& name) -> bool {
  if (name.extension() != ".wal" && name.extension() != ".sst") {
    return false;
  }
  const auto stem = name.stem().string();
  std::uint64_t number = 0;
  const auto [end, error] = std::from_chars(stem.data(), stem.data() + stem.size(), number);
  return error == std::errc{} && end == stem.data() + stem.size() && stem == std::to_string(number);
}
}  // namespace

auto RemoveObsolete(const std::filesystem::path& path) -> bool {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    std::clog << "Deferred obsolete-file cleanup: " << path << ": " << error.message() << '\n';
  }
  // Removal need not be durable: reappearing unreferenced files are retried at Open.
  return !error;
}

void CleanupUnreferenced(const std::filesystem::path& directory, const ManifestState& state) {
  std::set<std::filesystem::path> live{WalPath({}, state.metadata.current_wal)};
  if (state.metadata.previous_wal != 0) {
    live.insert(WalPath({}, state.metadata.previous_wal));
  }
  for (const auto& file : state.version->files) {
    live.insert(TablePath({}, file.number));
  }
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end; it != end && !error;
       it.increment(error)) {
    const auto name = it->path().filename();
    bool obsolete = name == "WAL" || (Numbered(name) && !live.contains(name));
    if (name.extension() == ".tmp") {
      const auto target = name.stem();
      obsolete = target == "CURRENT" || target == "MANIFEST-1" || Numbered(target);
    }
    if (obsolete) {
      static_cast<void>(RemoveObsolete(it->path()));
    }
  }
  if (error) {
    std::clog << "Deferred directory cleanup: " << directory << ": " << error.message() << '\n';
  }
}
}  // namespace lsm::internal
