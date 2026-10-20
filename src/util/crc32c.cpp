#include "util/crc32c.hpp"

#include <array>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <nmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace lsm::internal {
namespace {

constexpr auto MakeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    auto remainder = i;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder >> 1) ^ ((remainder & 1U) != 0 ? 0x82f63b78U : 0U);
    }
    table[i] = remainder;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

std::uint32_t ExtendCrc32cTable(std::uint32_t crc, std::string_view bytes) noexcept {
  auto remainder = ~crc;
  for (const char byte : bytes) {
    remainder = kTable[(remainder ^ static_cast<unsigned char>(byte)) & 0xffU] ^ (remainder >> 8);
  }
  return ~remainder;
}

bool HasSse42Crc32c() noexcept {
#if defined(_M_X64)
  int registers[4]{};
  __cpuid(registers, 1);
  return (registers[2] & (1 << 20)) != 0;
#elif defined(__x86_64__)
  return __builtin_cpu_supports("sse4.2") != 0;
#else
  return false;
#endif
}

#if defined(__x86_64__) && !defined(_MSC_VER)
__attribute__((target("sse4.2")))
#endif
std::uint32_t
ExtendCrc32cSse42(std::uint32_t crc, std::string_view bytes) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  std::uint64_t remainder = static_cast<std::uint32_t>(~crc);
  while (bytes.size() >= sizeof(std::uint64_t)) {
    std::uint64_t word = 0;
    std::memcpy(&word, bytes.data(), sizeof(word));
    remainder = _mm_crc32_u64(remainder, word);
    bytes.remove_prefix(sizeof(word));
  }
  auto tail = static_cast<std::uint32_t>(remainder);
  for (const char byte : bytes) {
    tail = _mm_crc32_u8(tail, static_cast<unsigned char>(byte));
  }
  return ~tail;
#else
  return ExtendCrc32cTable(crc, bytes);
#endif
}

std::uint32_t ExtendCrc32c(std::uint32_t crc, std::string_view bytes) noexcept {
  static const auto extend = HasSse42Crc32c() ? ExtendCrc32cSse42 : ExtendCrc32cTable;
  return extend(crc, bytes);
}

}  // namespace lsm::internal
