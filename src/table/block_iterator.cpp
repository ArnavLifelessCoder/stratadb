#include "table/block_iterator.hpp"

#include <limits>

#include "table/format.hpp"
#include "util/varint.hpp"

namespace lsm::internal::table {
BlockIterator::BlockIterator(std::string_view bytes) : bytes_(bytes) {
  if (bytes.size() < 8 || !ChecksumValid(bytes)) {
    Fail("block iterator CRC or size");
    return;
  }
  count_ = static_cast<std::size_t>(LoadFixed(bytes.substr(bytes.size() - 8), 4));
  if (count_ > (bytes.size() - 8) / 4) {
    Fail("block iterator restart count");
    return;
  }
  data_size_ = bytes.size() - 8 - count_ * 4;
  if ((count_ == 0) != (data_size_ == 0)) {
    Fail("block iterator empty layout");
    return;
  }
  for (std::size_t i = 0; i < count_; ++i) {
    if ((i == 0 && Offset(i) != 0) || Offset(i) >= Offset(i + 1) || Offset(i + 1) > data_size_) {
      Fail("block iterator restart offset");
      return;
    }
  }
}

void BlockIterator::Fail(const char* message) {
  status_ = Status::Corruption(message);
  valid_ = false;
}

auto BlockIterator::Offset(std::size_t restart) const -> std::size_t {
  return restart == count_
             ? data_size_
             : static_cast<std::size_t>(LoadFixed(bytes_.substr(data_size_ + restart * 4), 4));
}

auto BlockIterator::Decode(std::size_t offset, std::string_view previous, std::string& key,
                           std::string_view& value, std::size_t& next) -> bool {
  auto input = bytes_.substr(offset, data_size_ - offset);
  std::uint32_t shared = 0, unshared = 0, length = 0;
  if (!ConsumeVarint32(input, shared) || !ConsumeVarint32(input, unshared) ||
      !ConsumeVarint32(input, length) || shared > previous.size() || unshared > input.size() ||
      length > input.size() - unshared ||
      static_cast<std::uint64_t>(shared) + unshared > std::numeric_limits<std::uint32_t>::max()) {
    Fail("block iterator entry length");
    return false;
  }
  std::string decoded(previous.substr(0, shared));
  decoded.append(input.substr(0, unshared));
  input.remove_prefix(unshared);
  if (!ValidKey(decoded) || (!previous.empty() && Compare(previous, decoded) >= 0)) {
    Fail("block iterator key order");
    return false;
  }
  key = std::move(decoded);
  value = input.substr(0, length);
  input.remove_prefix(length);
  next = data_size_ - input.size();
  ++decoded_;
  return true;
}

void BlockIterator::Restart(std::size_t group) {
  valid_ = false;
  if (!status_.ok() || group >= count_) return;
  group_ = group;
  within_ = 1;
  offset_ = Offset(group);
  valid_ = Decode(offset_, {}, key_, value_, next_);
  if (valid_ && next_ > Offset(group + 1)) Fail("block iterator crosses restart");
}

void BlockIterator::SeekToFirst() { Restart(0); }

void BlockIterator::SeekToLast() {
  Restart(count_ == 0 ? 0 : count_ - 1);
  while (Valid() && next_ < data_size_) Next();
}

void BlockIterator::Seek(std::string_view target) {
  valid_ = false;
  if (!status_.ok() || count_ == 0) return;
  if (target.size() < 8) {
    status_ = Status::InvalidArgument("short iterator seek key");
    return;
  }
  std::size_t first = 0, last = count_;
  while (first < last) {
    const auto middle = first + (last - first) / 2;
    std::string key;
    std::string_view value;
    std::size_t next = 0;
    if (!Decode(Offset(middle), {}, key, value, next)) return;
    if (next > Offset(middle + 1)) {
      Fail("block iterator crosses restart");
      return;
    }
    if (Compare(key, target) <= 0)
      first = middle + 1;
    else
      last = middle;
  }
  Restart(first == 0 ? 0 : first - 1);
  while (Valid() && Compare(key_, target) < 0) Next();
}

void BlockIterator::Next() {
  if (!Valid()) return;
  if (next_ == Offset(group_ + 1)) {
    if (group_ + 1 == count_) {
      valid_ = false;
      return;
    }
    if (within_ != 16) {
      Fail("block iterator short restart group");
      return;
    }
    const auto previous = key_;
    Restart(group_ + 1);
    if (Valid() && Compare(previous, key_) >= 0) Fail("block iterator restart key order");
    return;
  }
  if (++within_ > 16 || next_ > Offset(group_ + 1)) {
    Fail("block iterator restart interval");
    return;
  }
  offset_ = next_;
  valid_ = Decode(offset_, key_, key_, value_, next_);
  if (valid_ && next_ > Offset(group_ + 1)) Fail("block iterator crosses restart");
}

void BlockIterator::Prev() {
  if (!Valid()) return;
  const auto end = offset_;
  if (end == 0) {
    valid_ = false;
    return;
  }
  Restart(within_ == 1 ? group_ - 1 : group_);
  while (Valid() && next_ < end) Next();
  if (Valid() && next_ != end) Fail("block iterator reverse boundary");
}
}  // namespace lsm::internal::table
