#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "lsm/status.hpp"

namespace lsm::platform {
// Read-only mapping of an immutable file. Views live until this object is destroyed.
// Concurrent readers are safe. Callers must not modify or truncate a mapped file.
class MappedFile {
 public:
  virtual ~MappedFile() = default;
  static auto Open(const std::filesystem::path& path, std::unique_ptr<MappedFile>& output)
      -> Status;
  [[nodiscard]] virtual auto Bytes() const -> std::string_view = 0;
};
}  // namespace lsm::platform
