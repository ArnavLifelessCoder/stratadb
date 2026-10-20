#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "lsm/status.hpp"

namespace lsm::platform {

// Calls on a handle are externally serialized. The virtual boundary permits
// deterministic short-write and sync-failure tests without changing DB's API.
class File {
 public:
  virtual ~File() = default;
  static auto Open(const std::filesystem::path& path, bool create, std::unique_ptr<File>& out)
      -> Status;
  virtual auto ReadAll(std::string& output) -> Status = 0;
  virtual auto Append(std::string_view bytes) -> Status = 0;
  virtual auto Sync() -> Status = 0;
  virtual auto Truncate(std::uint64_t size) -> Status = 0;
  virtual auto Lock() -> Status = 0;
};

// Creates missing directories and persists their entries in their parents.
auto CreateDirectories(const std::filesystem::path& path) -> Status;
// Caller must Sync the source file first. Both paths must be on the same filesystem.
auto AtomicRename(const std::filesystem::path& from, const std::filesystem::path& to) -> Status;

}  // namespace lsm::platform
