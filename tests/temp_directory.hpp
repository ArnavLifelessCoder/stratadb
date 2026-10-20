#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace lsm::test {
class TempDirectory {
 public:
  TempDirectory() {
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    // Keep all test data under the build directory, including on hosted runners.
    const auto root = std::filesystem::current_path() / "test-data";
    std::filesystem::create_directories(root);
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = root / (std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
      if (std::filesystem::create_directory(path_)) {
        return;
      }
    }
    throw std::runtime_error("cannot create unique test directory");
  }
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};
}  // namespace lsm::test
