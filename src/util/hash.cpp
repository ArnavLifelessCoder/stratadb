#include "util/hash.hpp"

#include <bit>
#include <cstddef>

namespace lsm::internal {
namespace {

constexpr std::uint64_t kC1 = 0x87c37b91114253d5ULL;
constexpr std::uint64_t kC2 = 0x4cf5ad432745937fULL;

auto LoadLittleEndian(std::string_view bytes) noexcept -> std::uint64_t {
  std::uint64_t word = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    word |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return word;
}

auto MixFirst(std::uint64_t word) noexcept -> std::uint64_t {
  return std::rotl(word * kC1, 31) * kC2;
}

auto MixSecond(std::uint64_t word) noexcept -> std::uint64_t {
  return std::rotl(word * kC2, 33) * kC1;
}

auto Finalize(std::uint64_t word) noexcept -> std::uint64_t {
  word ^= word >> 33;
  word *= 0xff51afd7ed558ccdULL;
  word ^= word >> 33;
  word *= 0xc4ceb9fe1a85ec53ULL;
  return word ^ (word >> 33);
}

}  // namespace

auto Hash64(std::string_view bytes, std::uint32_t seed) noexcept -> std::uint64_t {
  // Algorithm: Austin Appleby's public-domain MurmurHash3 x64_128.
  // https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
  std::uint64_t first = seed;
  std::uint64_t second = seed;
  auto remaining = bytes;
  while (remaining.size() >= 16) {
    first ^= MixFirst(LoadLittleEndian(remaining.substr(0, 8)));
    first = (std::rotl(first, 27) + second) * 5 + 0x52dce729U;
    second ^= MixSecond(LoadLittleEndian(remaining.substr(8, 8)));
    second = (std::rotl(second, 31) + first) * 5 + 0x38495ab5U;
    remaining.remove_prefix(16);
  }
  if (remaining.size() > 8) {
    second ^= MixSecond(LoadLittleEndian(remaining.substr(8)));
    remaining = remaining.substr(0, 8);
  }
  first ^= MixFirst(LoadLittleEndian(remaining));
  first ^= static_cast<std::uint64_t>(bytes.size());
  second ^= static_cast<std::uint64_t>(bytes.size());
  first += second;
  second += first;
  return Finalize(first) + Finalize(second);
}

}  // namespace lsm::internal
