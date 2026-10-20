#include "lsm/status.hpp"

namespace lsm {

auto Status::ToString() const -> std::string {
  std::string result;
  switch (code_) {
    case StatusCode::kOk:
      return "OK";
    case StatusCode::kNotFound:
      result = "NotFound";
      break;
    case StatusCode::kInvalidArgument:
      result = "InvalidArgument";
      break;
    case StatusCode::kCorruption:
      result = "Corruption";
      break;
    case StatusCode::kIOError:
      result = "IOError";
      break;
  }
  if (!message_.empty()) {
    result += ": ";
    result += message_;
  }
  return result;
}

}  // namespace lsm
