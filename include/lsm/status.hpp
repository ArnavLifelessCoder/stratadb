#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace lsm {

// Preserve the public error-code representation established in M1.
// NOLINTNEXTLINE(performance-enum-size)
enum class StatusCode : int { kOk, kNotFound, kInvalidArgument, kCorruption, kIOError };

// Success has no message. Errors carry an owned message, including embedded NULs.
class [[nodiscard]] Status {
 public:
  Status() = default;
  static auto NotFound(std::string message) -> Status {
    return {StatusCode::kNotFound, std::move(message)};
  }
  static auto InvalidArgument(std::string message) -> Status {
    return {StatusCode::kInvalidArgument, std::move(message)};
  }
  static auto Corruption(std::string message) -> Status {
    return {StatusCode::kCorruption, std::move(message)};
  }
  static auto IOError(std::string message) -> Status {
    return {StatusCode::kIOError, std::move(message)};
  }

  [[nodiscard]] auto ok() const noexcept -> bool { return code_ == StatusCode::kOk; }
  [[nodiscard]] auto IsNotFound() const noexcept -> bool { return code_ == StatusCode::kNotFound; }
  [[nodiscard]] auto code() const noexcept -> StatusCode { return code_; }
  [[nodiscard]] auto message() const noexcept -> std::string_view { return message_; }
  [[nodiscard]] auto ToString() const -> std::string;

 private:
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

}  // namespace lsm
