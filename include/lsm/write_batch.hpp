#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/status.hpp"

namespace lsm {

// Owns its keys and values. Operations are applied in insertion order atomically.
// A batch must not be modified while DB::Write uses it.
class WriteBatch {
 public:
  enum class Type : unsigned char { kDelete = 0, kPut = 1 };
  struct Entry {
    Type type;
    std::string key;
    std::string value;
  };

  Status Put(std::string_view key, std::string_view value);
  Status Delete(std::string_view key);
  void Clear() noexcept { entries_.clear(); }
  [[nodiscard]] std::size_t Count() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::vector<Entry>& Entries() const noexcept { return entries_; }

 private:
  std::vector<Entry> entries_;
};

}  // namespace lsm
