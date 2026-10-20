#include "write_batch.hpp"

#include <limits>
#include <utility>

#include "util/varint.hpp"

namespace lsm {
namespace {
constexpr auto kMaxLength = std::numeric_limits<std::uint32_t>::max();
}

auto WriteBatch::Put(std::string_view key, std::string_view value) -> Status {
  if (key.size() > kMaxLength || value.size() > kMaxLength || Count() >= kMaxLength) {
    return Status::InvalidArgument("batch exceeds 32-bit format limits");
  }
  entries_.push_back({.type = Type::kPut, .key = std::string(key), .value = std::string(value)});
  return {};
}

auto WriteBatch::Delete(std::string_view key) -> Status {
  if (key.size() > kMaxLength || Count() >= kMaxLength) {
    return Status::InvalidArgument("batch exceeds 32-bit format limits");
  }
  entries_.push_back({.type = Type::kDelete, .key = std::string(key), .value = {}});
  return {};
}

namespace internal {
namespace {
auto ConsumeString(std::string_view& input, std::string_view& value) -> bool {
  std::uint32_t length = 0;
  if (!ConsumeVarint32(input, length) || length > input.size()) {
    return false;
  }
  value = input.substr(0, length);
  input.remove_prefix(length);
  return true;
}
}  // namespace

auto EncodeBatch(const WriteBatch& batch, std::uint64_t sequence, std::string& output) -> Status {
  if (batch.Count() == 0 || sequence == 0 || sequence > kMaxSequence ||
      batch.Count() - 1 > kMaxSequence - sequence) {
    return Status::InvalidArgument("invalid batch sequence range");
  }
  // Check the total before allocating. Include a conservative bound for all varints.
  std::uint64_t size = 14;
  for (const auto& entry : batch.Entries()) {
    size += 11 + static_cast<std::uint64_t>(entry.key.size()) + entry.value.size();
    if (size > kMaxLength) {
      return Status::InvalidArgument("WAL payload exceeds 32-bit length");
    }
  }
  std::string encoded;
  AppendVarint64(encoded, sequence);
  AppendVarint32(encoded, static_cast<std::uint32_t>(batch.Count()));
  for (const auto& entry : batch.Entries()) {
    encoded.push_back(static_cast<char>(entry.type));
    AppendVarint32(encoded, static_cast<std::uint32_t>(entry.key.size()));
    encoded.append(entry.key);
    if (entry.type == WriteBatch::Type::kPut) {
      AppendVarint32(encoded, static_cast<std::uint32_t>(entry.value.size()));
      encoded.append(entry.value);
    }
  }
  output = std::move(encoded);
  return {};
}

auto DecodeBatch(std::string_view input, std::uint64_t& sequence, WriteBatch& batch) -> Status {
  std::uint64_t start = 0;
  std::uint32_t count = 0;
  if (!ConsumeVarint64(input, start) || !ConsumeVarint32(input, count) || count == 0 ||
      start == 0 || start > kMaxSequence || count - 1 > kMaxSequence - start ||
      count > input.size() / 2) {
    return Status::Corruption("invalid batch header");
  }
  WriteBatch decoded;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (input.empty()) {
      return Status::Corruption("missing batch operation");
    }
    const auto type = static_cast<WriteBatch::Type>(input.front());
    input.remove_prefix(1);
    std::string_view key;
    std::string_view value;
    if (!ConsumeString(input, key)) {
      return Status::Corruption("truncated batch key");
    }
    Status status;
    if (type == WriteBatch::Type::kPut) {
      if (!ConsumeString(input, value)) {
        return Status::Corruption("truncated batch value");
      }
      status = decoded.Put(key, value);
    } else if (type == WriteBatch::Type::kDelete) {
      status = decoded.Delete(key);
    } else {
      return Status::Corruption("unknown batch operation");
    }
    if (!status.ok()) {
      return status;
    }
  }
  if (!input.empty()) {
    return Status::Corruption("trailing batch bytes");
  }
  sequence = start;
  batch = std::move(decoded);
  return {};
}

}  // namespace internal
}  // namespace lsm
