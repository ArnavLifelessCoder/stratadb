#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace lsm {

enum class StatusCode { kOk, kNotFound, kInvalidArgument, kCorruption, kIOError };

// Success has no message. Errors carry an owned message, including embedded NULs.
class [[nodiscard]] Status {
 public:
  Status() = default;
  static Status NotFound(std::string message) {
    return Status(StatusCode::kNotFound, std::move(message));
  }
  static Status InvalidArgument(std::string message) {
    return Status(StatusCode::kInvalidArgument, std::move(message));
  }
  static Status Corruption(std::string message) {
    return Status(StatusCode::kCorruption, std::move(message));
  }
  static Status IOError(std::string message) {
    return Status(StatusCode::kIOError, std::move(message));
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
  [[nodiscard]] bool IsNotFound() const noexcept { return code_ == StatusCode::kNotFound; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  [[nodiscard]] std::string ToString() const;

 private:
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

}  // namespace lsm
