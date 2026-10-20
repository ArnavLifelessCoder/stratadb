#include "table/block.hpp"

#include <algorithm>
#include <limits>

#include "table/format.hpp"
#include "util/varint.hpp"

namespace lsm::internal::table {
namespace {
constexpr std::size_t kRestartInterval = 16;
constexpr auto kMax32 = std::numeric_limits<std::uint32_t>::max();
}  // namespace

auto EncodeBlock(std::span<const Entry> entries, std::string& output) -> Status {
  std::string encoded;
  std::vector<std::uint32_t> restarts;
  std::string_view previous;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    if (!ValidKey(entry.key) || (index != 0 && Compare(previous, entry.key) >= 0) ||
        entry.key.size() > kMax32 || entry.value.size() > kMax32) {
      return Status::InvalidArgument("invalid or unordered block entry");
    }
    std::size_t shared = 0;
    if (index % kRestartInterval == 0) {
      if (encoded.size() > kMax32) {
        return Status::InvalidArgument("block offset overflow");
      }
      restarts.push_back(static_cast<std::uint32_t>(encoded.size()));
    } else {
      while (shared < std::min(previous.size(), entry.key.size()) &&
             previous[shared] == entry.key[shared]) {
        ++shared;
      }
    }
    AppendVarint32(encoded, static_cast<std::uint32_t>(shared));
    AppendVarint32(encoded, static_cast<std::uint32_t>(entry.key.size() - shared));
    AppendVarint32(encoded, static_cast<std::uint32_t>(entry.value.size()));
    encoded.append(entry.key, shared);
    encoded.append(entry.value);
    previous = entry.key;
  }
  if (restarts.size() > kMax32 || encoded.size() > kMax32 ||
      restarts.size() > (kMax32 - encoded.size()) / 4) {
    return Status::InvalidArgument("block size overflow");
  }
  for (const auto offset : restarts) {
    AppendFixed<4>(encoded, offset);
  }
  AppendFixed<4>(encoded, restarts.size());
  Seal(encoded);
  output = std::move(encoded);
  return {};
}

auto DecodeBlock(std::string_view bytes, std::vector<Entry>& output) -> Status {
  if (bytes.size() < 8 || !ChecksumValid(bytes)) {
    return Status::Corruption("block CRC or size");
  }
  const auto count = LoadFixed(bytes.substr(bytes.size() - 8), 4);
  if (count > (bytes.size() - 8) / 4) {
    return Status::Corruption("block restart count");
  }
  const auto data_size = bytes.size() - 8 - static_cast<std::size_t>(count) * 4;
  auto input = bytes.substr(0, data_size);
  std::vector<Entry> entries;
  std::size_t restart = 0;
  while (!input.empty()) {
    const auto offset = data_size - input.size();
    std::uint32_t shared = 0;
    std::uint32_t unshared = 0;
    std::uint32_t length = 0;
    if (!ConsumeVarint32(input, shared) || !ConsumeVarint32(input, unshared) ||
        !ConsumeVarint32(input, length)) {
      return Status::Corruption("block entry varints");
    }
    if (entries.size() % kRestartInterval == 0) {
      if (restart >= count || shared != 0 ||
          LoadFixed(bytes.substr(data_size + restart * 4), 4) != offset) {
        return Status::Corruption("block restart offset");
      }
      ++restart;
    }
    const std::string_view previous = entries.empty() ? std::string_view{} : entries.back().key;
    if (shared > previous.size() || unshared > input.size() || length > input.size() - unshared ||
        static_cast<std::uint64_t>(shared) + unshared > kMax32) {
      return Status::Corruption("block entry length");
    }
    Entry entry{.key = std::string(previous.substr(0, shared)), .value = {}};
    entry.key.append(input.substr(0, unshared));
    input.remove_prefix(unshared);
    entry.value = input.substr(0, length);
    input.remove_prefix(length);
    if (!ValidKey(entry.key) || (!entries.empty() && Compare(previous, entry.key) >= 0)) {
      return Status::Corruption("block key ordering");
    }
    entries.push_back(std::move(entry));
  }
  if (restart != count) {
    return Status::Corruption("unused block restarts");
  }
  output = std::move(entries);
  return {};
}

auto SeekBlock(std::string_view bytes, std::string_view target, Entry& output,
               std::size_t* decoded_entries) -> Status {
  if (decoded_entries != nullptr) {
    *decoded_entries = 0;
  }
  if (target.size() < 8) {
    return Status::InvalidArgument("short block seek key");
  }
  if (bytes.size() < 8 || !ChecksumValid(bytes)) {
    return Status::Corruption("block CRC or size");
  }
  const auto count = static_cast<std::size_t>(LoadFixed(bytes.substr(bytes.size() - 8), 4));
  if (count > (bytes.size() - 8) / 4) {
    return Status::Corruption("block restart count");
  }
  const auto data_size = bytes.size() - 8 - count * 4;
  const auto offset = [&](std::size_t index) {
    return index == count
               ? data_size
               : static_cast<std::size_t>(LoadFixed(bytes.substr(data_size + index * 4), 4));
  };
  if ((count == 0) != (data_size == 0)) {
    return Status::Corruption("block empty restart layout");
  }
  for (std::size_t i = 0; i < count; ++i) {
    if ((i == 0 && offset(i) != 0) || offset(i) >= offset(i + 1) || offset(i + 1) > data_size) {
      return Status::Corruption("block restart offset");
    }
  }
  if (count == 0) {
    return Status::NotFound("empty block");
  }
  const auto decode = [&](std::string_view& input, std::string_view previous,
                          Entry& entry) -> Status {
    std::uint32_t shared = 0;
    std::uint32_t unshared = 0;
    std::uint32_t length = 0;
    if (!ConsumeVarint32(input, shared) || !ConsumeVarint32(input, unshared) ||
        !ConsumeVarint32(input, length) || shared > previous.size() || unshared > input.size() ||
        length > input.size() - unshared ||
        static_cast<std::uint64_t>(shared) + unshared > kMax32) {
      return Status::Corruption("block seek entry");
    }
    entry.key.assign(previous.substr(0, shared));
    entry.key.append(input.substr(0, unshared));
    input.remove_prefix(unshared);
    entry.value = input.substr(0, length);
    input.remove_prefix(length);
    if (!ValidKey(entry.key) || (!previous.empty() && Compare(previous, entry.key) >= 0)) {
      return Status::Corruption("block seek key ordering");
    }
    if (decoded_entries != nullptr) {
      ++*decoded_entries;
    }
    return {};
  };
  const auto restart = [&](std::size_t index, Entry& entry) {
    auto input = bytes.substr(offset(index), offset(index + 1) - offset(index));
    return decode(input, {}, entry);
  };
  std::size_t first = 0;
  std::size_t last = count;
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    Entry entry;
    auto status = restart(middle, entry);
    if (!status.ok()) {
      return status;
    }
    if (Compare(entry.key, target) <= 0) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  const auto group = first == 0 ? 0 : first - 1;
  auto input = bytes.substr(offset(group), offset(group + 1) - offset(group));
  std::string previous;
  std::size_t entries = 0;
  while (!input.empty()) {
    if (++entries > kRestartInterval) {
      return Status::Corruption("block restart interval");
    }
    Entry entry;
    auto status = decode(input, previous, entry);
    if (!status.ok()) {
      return status;
    }
    if (Compare(entry.key, target) >= 0) {
      output = std::move(entry);
      return {};
    }
    previous = std::move(entry.key);
  }
  if (group + 1 == count) {
    return Status::NotFound("past block");
  }
  if (entries != kRestartInterval) {
    return Status::Corruption("short restart group");
  }
  Entry entry;
  auto status = restart(group + 1, entry);
  if (!status.ok()) {
    return status;
  }
  if (Compare(previous, entry.key) >= 0 || Compare(entry.key, target) < 0) {
    return Status::Corruption("block restart key ordering");
  }
  output = std::move(entry);
  return {};
}
}  // namespace lsm::internal::table
