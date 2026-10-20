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
  static Status Open(const std::filesystem::path& path, bool create, std::unique_ptr<File>& out);
  virtual Status ReadAll(std::string& output) = 0;
  virtual Status Append(std::string_view bytes) = 0;
  virtual Status Sync() = 0;
  virtual Status Truncate(std::uint64_t size) = 0;
  virtual Status Lock() = 0;
};

// Creates missing directories and persists their entries in their parents.
Status CreateDirectories(const std::filesystem::path& path);
// Caller must Sync the source file first. Both paths must be on the same filesystem.
Status AtomicRename(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace lsm::platform
