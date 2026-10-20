#pragma once

#include <string_view>

#include "lsm/status.hpp"

namespace lsm {
// Stable ordered view. Destroy iterators before their DB. One caller owns each
// iterator; independent iterators may run concurrently. key/value views remain
// valid until the iterator moves or is destroyed. Invalid navigation is a no-op.
class Iterator {
 public:
  virtual ~Iterator() = default;
  [[nodiscard]] virtual auto Valid() const -> bool = 0;
  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  virtual void Prev() = 0;
  [[nodiscard]] virtual auto key() const -> std::string_view = 0;
  [[nodiscard]] virtual auto value() const -> std::string_view = 0;
  [[nodiscard]] virtual auto status() const -> Status = 0;
};
}  // namespace lsm
