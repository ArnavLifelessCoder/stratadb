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

std::uint32_t Fixed32(std::string_view bytes) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return value;
}

// A damaged length must not make a valid later batch disappear as a "torn tail".
bool HasLaterRecord(std::string_view bytes, std::uint64_t last_sequence) {
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

Status Wal::Append(const WriteBatch& batch, std::uint64_t sequence, bool sync) {
  if (!failure_.ok()) {
    return failure_;
  }
  std::string payload;
  auto status = EncodeBatch(batch, sequence, payload);
  if (!status.ok()) {
    return status;
  }
  std::string body;
  AppendFixed32(body, static_cast<std::uint32_t>(payload.size()));
  body.push_back(kBatchRecord);
  body.append(payload);
  std::string record;
  AppendFixed32(record, Crc32c(body));
  record.append(body);
  failure_ = file_->Append(record);
  if (!failure_.ok()) {
    return failure_;
  }
  bytes_written_ += record.size();
  if (sync) {
    failure_ = file_->Sync();
    if (failure_.ok()) {
      ++sync_count_;
    }
  }
  return failure_;
}

Status Wal::RepairTail(std::size_t offset, std::size_t size) {
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

Status Wal::Replay(MemTable& table, std::uint64_t& last_sequence) {
  std::string storage;
  auto status = file_->ReadAll(storage);
  if (!status.ok()) {
    return status;
  }
  std::string_view remaining(storage);
  std::uint64_t last = 0;
  while (!remaining.empty()) {
    const auto offset = storage.size() - remaining.size();
    if (remaining.size() < kHeaderSize) {
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
