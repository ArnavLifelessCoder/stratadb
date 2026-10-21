#pragma once

#include <cstddef>
#include <string>

#include "internal_iterator.hpp"

namespace lsm::internal::table {
// Borrows one CRC-checked block. The owner must retain its bytes. Only the
// current reconstructed key is owned; values remain views into the block.
class BlockIterator final : public InternalIterator {
 public:
  // checksum_verified skips the CRC for a block that came from the block cache.
  explicit BlockIterator(std::string_view bytes, bool checksum_verified = false);
  [[nodiscard]] auto Valid() const -> bool override { return valid_ && status_.ok(); }
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(std::string_view target) override;
  void Next() override;
  void Prev() override;
  [[nodiscard]] auto key() const -> std::string_view override {
    return Valid() ? key_ : std::string_view{};
  }
  [[nodiscard]] auto value() const -> std::string_view override {
    return Valid() ? value_ : std::string_view{};
  }
  [[nodiscard]] auto status() const -> Status override { return status_; }
  [[nodiscard]] auto DecodedEntries() const -> std::size_t { return decoded_; }

 private:
  [[nodiscard]] auto Offset(std::size_t restart) const -> std::size_t;
  auto Decode(std::size_t offset, std::string_view previous, std::string& key,
              std::string_view& value, std::size_t& next) -> bool;
  void Restart(std::size_t group);
  void Fail(const char* message);
  std::string_view bytes_;
  std::size_t data_size_ = 0;
  std::size_t count_ = 0;
  std::size_t group_ = 0;
  std::size_t within_ = 0;
  std::size_t offset_ = 0;
  std::size_t next_ = 0;
  std::size_t decoded_ = 0;
  std::string key_;
  std::string_view value_;
  Status status_;
  bool valid_ = false;
};
}  // namespace lsm::internal::table
