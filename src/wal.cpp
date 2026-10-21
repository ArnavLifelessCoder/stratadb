#include "wal.hpp"

#include <iostream>
#include <string_view>

#include "util/crc32c.hpp"

namespace lsm::internal {
namespace {
constexpr std::size_t kHeaderSize = 9;
constexpr char kBatchRecord = 1;

void AppendFixed32(std::string& output, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    output.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

auto Fixed32(std::string_view bytes) -> std::uint32_t {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return value;
}

// A damaged length must not make a valid later batch disappear as a "torn tail".
auto HasLaterRecord(std::string_view bytes, std::uint64_t last_sequence) -> bool {
  for (std::size_t offset = 1; offset + kHeaderSize <= bytes.size(); ++offset) {
    const auto candidate = bytes.substr(offset);
    const auto length = Fixed32(candidate.substr(4));
    if (candidate[8] != kBatchRecord || length > candidate.size() - kHeaderSize) {
      continue;
    }
    if (Fixed32(candidate) != Crc32c(candidate.substr(4, 5 + static_cast<std::size_t>(length)))) {
      continue;
    }
    std::uint64_t sequence = 0;
    WriteBatch batch;
    if (DecodeBatch(candidate.substr(kHeaderSize, length), sequence, batch).ok() &&
        sequence > last_sequence) {
      return true;
    }
  }
  return false;
}
}  // namespace

auto Wal::Append(const WriteBatch& batch, std::uint64_t sequence, bool sync) -> Status {
  if (!failure_.ok()) {
    return failure_;
  }
  auto status = EncodeBatch(batch, sequence, payload_);
  if (!status.ok()) {
    return status;
  }
  // crc32c (4) | length (4) | type (1) | payload, built in a reused buffer; the
  // CRC covers length, type and payload and is filled in last.
  record_.assign(4, '\0');
  AppendFixed32(record_, static_cast<std::uint32_t>(payload_.size()));
  record_.push_back(kBatchRecord);
  record_.append(payload_);
  const auto crc = Crc32c(std::string_view(record_).substr(4));
  for (unsigned i = 0; i < 4; ++i) {
    record_[i] = static_cast<char>((crc >> (8 * i)) & 0xffU);
  }
  failure_ = file_->Append(record_);
  if (!failure_.ok()) {
    return failure_;
  }
  bytes_written_.fetch_add(record_.size(), std::memory_order_relaxed);
  if (sync) {
    return Sync();
  }
  return failure_;
}

auto Wal::Sync() -> Status {
  if (!failure_.ok()) {
    return failure_;
  }
  failure_ = file_->Sync();
  if (failure_.ok()) {
    sync_count_.fetch_add(1, std::memory_order_relaxed);
  }
  return failure_;
}

auto Wal::RepairTail(std::size_t offset, std::size_t size) -> Status {
  auto status = file_->Truncate(offset);
  if (status.ok()) {
    status = file_->Sync();
  }
  if (!status.ok()) {
    return status;
  }
  truncated_bytes_ = size - offset;
  std::clog << "WAL recovery: truncated " << truncated_bytes_ << " tail bytes at offset " << offset
            << '\n';
  return {};
}

auto Wal::Replay(MemTable& table, std::uint64_t& last_sequence, std::uint64_t first_sequence)
    -> Status {
  if (first_sequence == 0 || first_sequence > kMaxSequence + 1) {
    return Status::InvalidArgument("invalid WAL starting sequence");
  }
  std::string storage;
  auto status = file_->ReadAll(storage);
  if (!status.ok()) {
    return status;
  }
  std::string_view remaining(storage);
  std::uint64_t last = first_sequence - 1;
  while (!remaining.empty()) {
    const auto offset = storage.size() - remaining.size();
    // A power loss may leave allocated bytes zeroed instead of a short append.
    // Only accept zeros through EOF; zeros before another record are corruption.
    if (remaining.size() < kHeaderSize ||
        remaining.find_first_not_of('\0') == std::string_view::npos) {
      status = RepairTail(offset, storage.size());
      if (!status.ok()) {
        return status;
      }
      break;
    }
    const auto length = Fixed32(remaining.substr(4));
    if (length > remaining.size() - kHeaderSize) {
      if (HasLaterRecord(remaining, last)) {
        return Status::Corruption("WAL length damages a middle record");
      }
      status = RepairTail(offset, storage.size());
      if (!status.ok()) {
        return status;
      }
      break;
    }
    const std::size_t record_size = kHeaderSize + static_cast<std::size_t>(length);
    if (Fixed32(remaining) != Crc32c(remaining.substr(4, record_size - 4))) {
      if (record_size != remaining.size() || HasLaterRecord(remaining, last)) {
        return Status::Corruption("WAL CRC mismatch before end of file");
      }
      status = RepairTail(offset, storage.size());
      if (!status.ok()) {
        return status;
      }
      break;
    }
    if (remaining[8] != kBatchRecord) {
      return Status::Corruption("unknown WAL record type");
    }
    WriteBatch batch;
    std::uint64_t sequence = 0;
    status = DecodeBatch(remaining.substr(kHeaderSize, length), sequence, batch);
    if (!status.ok()) {
      return status;
    }
    if (sequence != last + 1) {
      return Status::Corruption("non-contiguous WAL sequences");
    }
    for (const auto& entry : batch.Entries()) {
      status = table.Insert(sequence++, entry.type, entry.key, entry.value);
      if (!status.ok()) {
        return status;
      }
    }
    last = sequence - 1;
    remaining.remove_prefix(record_size);
  }
  last_sequence = last;
  return {};
}
}  // namespace lsm::internal
