#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <system_error>

#include "platform/mapping.hpp"

namespace lsm::platform {
namespace {
auto MappingError() -> Status {
  return Status::IOError("map file: " + std::system_category().message(errno));
}

class PosixMapping final : public MappedFile {
 public:
  PosixMapping(void* data, std::size_t size) : data_(data), size_(size) {}
  ~PosixMapping() override {
    if (size_ != 0) {
      munmap(data_, size_);
    }
  }
  [[nodiscard]] auto Bytes() const -> std::string_view override {
    return size_ == 0 ? std::string_view{}
                      : std::string_view(static_cast<const char*>(data_), size_);
  }

 private:
  void* data_;
  std::size_t size_;
};
}  // namespace

auto MappedFile::Open(const std::filesystem::path& path, std::unique_ptr<MappedFile>& output)
    -> Status {
  const auto file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) {
    return MappingError();
  }
  struct stat info{};
  if (fstat(file, &info) != 0) {
    const auto status = MappingError();
    close(file);
    return status;
  }
  if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<std::uint64_t>(info.st_size) > std::numeric_limits<std::size_t>::max()) {
    close(file);
    return Status::IOError("mapping requires a regular file with representable size");
  }
  const auto size = static_cast<std::size_t>(info.st_size);
  void* data = nullptr;
  if (size != 0) {
    data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
    if (data == MAP_FAILED) {
      const auto status = MappingError();
      close(file);
      return status;
    }
  }
  close(file);
  output = std::make_unique<PosixMapping>(data, size);
  return {};
}
}  // namespace lsm::platform
