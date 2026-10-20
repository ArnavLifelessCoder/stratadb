#include "util/varint.hpp"

#include <cstddef>
#include <limits>

namespace lsm::internal {
namespace {

template <typename UInt>
void Append(std::string& destination, UInt value) {
  while (value >= 128) {
    destination.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
    value >>= 7;
  }
  destination.push_back(static_cast<char>(value));
}

template <typename UInt>
auto Consume(std::string_view& input, UInt& value) noexcept -> bool {
  constexpr auto kMax = std::numeric_limits<UInt>::max();
  constexpr std::size_t kMaxBytes = (std::numeric_limits<UInt>::digits + 6) / 7;
  UInt result = 0;
  for (std::size_t i = 0; i < input.size() && i < kMaxBytes; ++i) {
    const auto byte = static_cast<unsigned char>(input[i]);
    const auto payload = static_cast<UInt>(byte & 0x7fU);
    const auto shift = 7 * i;
    if (payload > (kMax >> shift)) {
      return false;
    }
    result |= payload << shift;
    if ((byte & 0x80U) == 0) {
      if (i != 0 && payload == 0) {
        return false;
      }
      value = result;
      input.remove_prefix(i + 1);
      return true;
    }
  }
  return false;
}

}  // namespace

void AppendVarint32(std::string& destination, std::uint32_t value) { Append(destination, value); }

void AppendVarint64(std::string& destination, std::uint64_t value) { Append(destination, value); }

auto ConsumeVarint32(std::string_view& input, std::uint32_t& value) noexcept -> bool {
  return Consume(input, value);
}

auto ConsumeVarint64(std::string_view& input, std::uint64_t& value) noexcept -> bool {
  return Consume(input, value);
}

}  // namespace lsm::internal
