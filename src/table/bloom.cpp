#include "table/bloom.hpp"

#include <algorithm>
#include <limits>

#include "table/format.hpp"
#include "util/hash.hpp"

namespace lsm::internal::table {
auto Bloom::Build(std::span<const std::string> keys, int bits_per_key, std::string& output)
    -> Status {
  if (bits_per_key < 0 || bits_per_key > 30) {
    return Status::InvalidArgument("bloom bits per key must be in [0,30]");
  }
  std::string encoded;
  if (bits_per_key == 0) {
    encoded.push_back('\0');
  } else {
    const auto bits = static_cast<std::size_t>(bits_per_key);
    if (keys.size() > (std::numeric_limits<std::size_t>::max() - 7) / bits) {
      return Status::InvalidArgument("bloom size overflow");
    }
    const auto size = std::max(std::size_t{8}, (keys.size() * bits + 7) / 8);
    if (size > encoded.max_size() - 5) {
      return Status::InvalidArgument("bloom too large");
    }
    const auto probes = static_cast<unsigned>(std::max(1, (bits_per_key * 69 + 50) / 100));
    encoded.assign(size + 1, '\0');
    encoded[0] = static_cast<char>(probes);
    const auto bit_count = static_cast<std::uint64_t>(size) * 8;
    for (const auto& key : keys) {
      const auto hash = Hash64(key);
      auto position = hash & 0xffffffffU;
      const auto step = (hash >> 32) | 1U;
      for (unsigned probe = 0; probe < probes; ++probe) {
        const auto bit = position % bit_count;
        auto& byte = encoded[1 + static_cast<std::size_t>(bit / 8)];
        byte = static_cast<char>(static_cast<unsigned char>(byte) | (1U << (bit % 8)));
        position += step;
      }
    }
  }
  Seal(encoded);
  output = std::move(encoded);
  return {};
}

auto Bloom::Open(std::string_view bytes, Bloom& output) -> Status {
  if (bytes.size() < 5 || !ChecksumValid(bytes)) {
    return Status::Corruption("bloom CRC or size");
  }
  const auto probes = static_cast<unsigned char>(bytes[0]);
  if (probes > 21 || (probes == 0 ? bytes.size() != 5 : bytes.size() < 13)) {
    return Status::Corruption("bloom probe count or bit array");
  }
  output.bits_ = bytes.substr(1, bytes.size() - 5);
  output.probes_ = probes;
  return {};
}

auto Bloom::MayContain(std::string_view key) const -> bool {
  if (probes_ == 0) {
    return true;
  }
  const auto hash = Hash64(key);
  auto position = hash & 0xffffffffU;
  const auto step = (hash >> 32) | 1U;
  const auto bit_count = static_cast<std::uint64_t>(bits_.size()) * 8;
  for (unsigned probe = 0; probe < probes_; ++probe) {
    const auto bit = position % bit_count;
    if ((static_cast<unsigned char>(bits_[static_cast<std::size_t>(bit / 8)]) &
         (1U << (bit % 8))) == 0) {
      return false;
    }
    position += step;
  }
  return true;
}
}  // namespace lsm::internal::table
