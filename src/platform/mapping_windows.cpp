#include "platform/mapping.hpp"

#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <limits>
#include <system_error>

namespace lsm::platform {
namespace {
auto MappingError() -> Status {
  const auto code = static_cast<int>(GetLastError());
  return Status::IOError("map file: " + std::system_category().message(code));
}

class WindowsMapping final : public MappedFile {
 public:
  WindowsMapping(HANDLE file, HANDLE mapping, const void* data, std::size_t size)
      : file_(file), mapping_(mapping), data_(data), size_(size) {}
  ~WindowsMapping() override {
    if (data_ != nullptr) {
      UnmapViewOfFile(data_);
    }
    if (mapping_ != nullptr) {
      CloseHandle(mapping_);
    }
    CloseHandle(file_);
  }
  [[nodiscard]] auto Bytes() const -> std::string_view override {
    return size_ == 0 ? std::string_view{}
                      : std::string_view(static_cast<const char*>(data_), size_);
  }

 private:
  HANDLE file_;
  HANDLE mapping_;
  const void* data_;
  std::size_t size_;
};
}  // namespace

auto MappedFile::Open(const std::filesystem::path& path, std::unique_ptr<MappedFile>& output)
    -> Status {
  const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return MappingError();
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) {
    const auto status = MappingError();
    CloseHandle(file);
    return status;
  }
  if (size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > std::numeric_limits<std::size_t>::max()) {
    CloseHandle(file);
    return Status::IOError("mapping size overflow");
  }
  HANDLE mapping = nullptr;
  const void* data = nullptr;
  if (size.QuadPart != 0) {
    mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
      const auto status = MappingError();
      CloseHandle(file);
      return status;
    }
    data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (data == nullptr) {
      const auto status = MappingError();
      CloseHandle(mapping);
      CloseHandle(file);
      return status;
    }
  }
  output = std::make_unique<WindowsMapping>(file, mapping, data,
                                            static_cast<std::size_t>(size.QuadPart));
  return {};
}
}  // namespace lsm::platform
