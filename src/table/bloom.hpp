#pragma once

#include <span>
#include <string>
#include <string_view>

#include "lsm/status.hpp"

namespace lsm::internal::table {
class Bloom {
 public:
  // Input is a set of user keys; callers deduplicate versions. Zero disables filtering.
  static auto Build(std::span<const std::string> keys, int bits_per_key, std::string& output)
      -> Status;
  static auto Open(std::string_view bytes, Bloom& output) -> Status;
  [[nodiscard]] auto MayContain(std::string_view key) const -> bool;

 private:
  std::string bits_;
  unsigned probes_ = 0;
};
}  // namespace lsm::internal::table
