#include "platform/file.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace lsm::platform {
namespace {
auto Error(std::string_view operation) -> Status {
  const auto code = static_cast<int>(GetLastError());
  return Status::IOError(std::string(operation) + ": " + std::system_category().message(code));
}

class WindowsFile final : public File {
 public:
  explicit WindowsFile(HANDLE handle) : handle_(handle) {}
  ~WindowsFile() override { CloseHandle(handle_); }

  auto ReadAll(std::string& output) -> Status override {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle_, &size)) {
      return Error("file size");
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > std::string{}.max_size()) {
      return Status::IOError("file is too large");
    }
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN)) {
      return Error("seek");
    }
    std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < data.size()) {
      const auto count = static_cast<DWORD>(std::min(data.size() - offset, std::size_t{1 << 20}));
      DWORD read = 0;
      if (!ReadFile(handle_, data.data() + offset, count, &read, nullptr)) {
        return Error("read");
      }
      if (read == 0) {
        return Status::IOError("unexpected end of file");
      }
      offset += read;
    }
    output = std::move(data);
    return {};
  }

  auto Append(std::string_view bytes) -> Status override {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle_, zero, nullptr, FILE_END)) {
      return Error("seek to append");
    }
    while (!bytes.empty()) {
      const auto count = static_cast<DWORD>(std::min(bytes.size(), std::size_t{1 << 20}));
      DWORD written = 0;
      if (!WriteFile(handle_, bytes.data(), count, &written, nullptr)) {
        return Error("write");
      }
      if (written == 0) {
        return Status::IOError("zero-byte write");
      }
      bytes.remove_prefix(written);
    }
    return {};
  }

  auto Sync() -> Status override {
    return FlushFileBuffers(handle_) ? Status{} : Error("FlushFileBuffers");
  }

  auto Truncate(std::uint64_t size) -> Status override {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
      return Status::InvalidArgument("truncate offset overflow");
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(handle_, offset, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) {
      return Error("truncate");
    }
    return {};
  }

  auto Lock() -> Status override {
    OVERLAPPED overlap{};
    return LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                      MAXDWORD, &overlap)
               ? Status{}
               : Error("database lock");
  }

 private:
  HANDLE handle_;
};
}  // namespace

auto File::Open(const std::filesystem::path& path, bool create, std::unique_ptr<File>& out)
    -> Status {
  const auto handle =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                  create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error("open");
  }
  out = std::make_unique<WindowsFile>(handle);
  return {};
}

auto AtomicRename(const std::filesystem::path& from, const std::filesystem::path& to) -> Status {
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
             ? Status{}
             : Error("atomic rename");
}

auto CreateDirectories(const std::filesystem::path& path) -> Status {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) {
    return Status::IOError("create directory: " + error.message());
  }
  return {};
}
}  // namespace lsm::platform
