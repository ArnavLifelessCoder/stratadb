#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/file.hpp"

namespace lsm::platform {
namespace {
Status Error(std::string_view operation) {
  const int code = errno;
  return Status::IOError(std::string(operation) + ": " + std::generic_category().message(code));
}

template <typename Operation>
int Retry(Operation operation) {
  int result = 0;
  do {
    result = operation();
  } while (result < 0 && errno == EINTR);
  return result;
}

Status SyncDirectory(const std::filesystem::path& path) {
  const int fd = Retry([&] { return open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); });
  if (fd < 0) {
    return Error("open directory");
  }
  const auto status = Retry([&] { return fsync(fd); }) == 0 ? Status{} : Error("directory fsync");
  close(fd);
  return status;
}

class PosixFile final : public File {
 public:
  explicit PosixFile(int fd) : fd_(fd) {}
  ~PosixFile() override { close(fd_); }

  Status ReadAll(std::string& output) override {
    struct stat info{};
    if (fstat(fd_, &info) != 0) {
      return Error("file size");
    }
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > std::string{}.max_size()) {
      return Status::IOError("file is too large");
    }
    std::string data(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < data.size()) {
      const auto count = std::min(data.size() - offset, std::size_t{1 << 20});
      const auto read = pread(fd_, data.data() + offset, count, static_cast<off_t>(offset));
      if (read < 0 && errno == EINTR) {
        continue;
      }
      if (read <= 0) {
        return read == 0 ? Status::IOError("unexpected end of file") : Error("read");
      }
      offset += static_cast<std::size_t>(read);
    }
    output = std::move(data);
    return {};
  }

  Status Append(std::string_view bytes) override {
    while (!bytes.empty()) {
      const auto written = write(fd_, bytes.data(), std::min(bytes.size(), std::size_t{1 << 20}));
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        return written == 0 ? Status::IOError("zero-byte write") : Error("write");
      }
      bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
  }

  Status Sync() override {
    return Retry([&] { return fdatasync(fd_); }) == 0 ? Status{} : Error("fdatasync");
  }

  Status Truncate(std::uint64_t size) override {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
      return Status::InvalidArgument("truncate offset overflow");
    }
    return Retry([&] { return ftruncate(fd_, static_cast<off_t>(size)); }) == 0 ? Status{}
                                                                                : Error("truncate");
  }

  Status Lock() override {
    return Retry([&] { return flock(fd_, LOCK_EX | LOCK_NB); }) == 0 ? Status{}
                                                                     : Error("database lock");
  }

 private:
  int fd_;
};
}  // namespace

Status File::Open(const std::filesystem::path& path, bool create, std::unique_ptr<File>& out) {
  const int fd = Retry([&] {
    return open(path.c_str(), O_RDWR | O_APPEND | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
  });
  if (fd < 0) {
    return Error("open");
  }
  out = std::make_unique<PosixFile>(fd);
  return {};
}

Status AtomicRename(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (rename(from.c_str(), to.c_str()) != 0) {
    return Error("atomic rename");
  }
  auto status =
      SyncDirectory(to.parent_path().empty() ? std::filesystem::path{"."} : to.parent_path());
  if (status.ok() && from.parent_path() != to.parent_path()) {
    status =
        SyncDirectory(from.parent_path().empty() ? std::filesystem::path{"."} : from.parent_path());
  }
  return status;
}

Status CreateDirectories(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    return Status::IOError("resolve directory: " + error.message());
  }
  std::vector<std::filesystem::path> missing;
  auto current = absolute;
  while (!std::filesystem::exists(current, error)) {
    if (error) {
      return Status::IOError("inspect directory: " + error.message());
    }
    missing.push_back(current);
    current = current.parent_path();
  }
  std::filesystem::create_directories(absolute, error);
  if (error) {
    return Status::IOError("create directory: " + error.message());
  }
  for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
    const auto status = SyncDirectory(it->parent_path());
    if (!status.ok()) {
      return status;
    }
  }
  return {};
}
}  // namespace lsm::platform
