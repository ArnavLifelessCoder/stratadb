#include "table/table.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

#include "table/format.hpp"

namespace lsm::internal::table {
namespace {
constexpr std::size_t kFooterSize = 48;
constexpr std::uint64_t kMagic = 0x3330415441525453ULL;  // STRATA03, little endian.
constexpr std::uint64_t kVersion = 1;

auto Validate(const TableOptions& options) -> Status {
  if (options.block_bytes == 0 ||
      options.block_bytes > std::numeric_limits<std::uint32_t>::max() - 8U ||
      options.bloom_bits_per_key < 0 || options.bloom_bits_per_key > 30) {
    return Status::InvalidArgument("invalid table block size or bloom bits per key");
  }
  return {};
}

auto Range(std::string_view bytes, std::string_view handle, std::string_view& output) -> bool {
  if (handle.size() != 16) {
    return false;
  }
  const auto offset = LoadFixed(handle, 8);
  const auto size = LoadFixed(handle.substr(8), 8);
  if (offset > bytes.size() || size > bytes.size() - offset) {
    return false;
  }
  output = bytes.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
  return true;
}
}  // namespace

TableWriter::TableWriter(platform::File& file, TableOptions options)
    : file_(file), options_(options), status_(Validate(options)) {}

auto TableWriter::Append(std::string_view bytes, std::string& handle) -> Status {
  if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - offset_) {
    return Status::InvalidArgument("table offset overflow");
  }
  auto status = file_.Append(bytes);
  if (!status.ok()) {
    return status;
  }
  handle.clear();
  AppendFixed<8>(handle, offset_);
  AppendFixed<8>(handle, bytes.size());
  offset_ += bytes.size();
  return {};
}

auto TableWriter::Flush() -> Status {
  if (pending_.empty()) {
    return {};
  }
  std::string bytes;
  auto status = EncodeBlock(pending_, bytes);
  if (!status.ok()) {
    return status;
  }
  std::string handle;
  status = Append(bytes, handle);
  if (!status.ok()) {
    return status;
  }
  // The exact last internal key is a valid upper separator, even when a user's
  // versions cross blocks. Avoid shortening a key across a sequence boundary.
  index_.push_back({.key = pending_.back().key, .value = std::move(handle)});
  pending_.clear();
  estimate_ = 0;
  return {};
}

auto TableWriter::Add(const Entry& entry) -> Status {
  if (!status_.ok()) {
    return status_;
  }
  if (finished_) {
    return Status::InvalidArgument("table is already finished");
  }
  if (!ValidKey(entry.key) || (!last_key_.empty() && Compare(last_key_, entry.key) >= 0) ||
      entry.key.size() > std::numeric_limits<std::uint32_t>::max() ||
      entry.value.size() > std::numeric_limits<std::uint32_t>::max() ||
      ((Tag(entry.key) & 0xffU) == 0 && !entry.value.empty())) {
    status_ = Status::InvalidArgument("invalid or unordered table entry");
    return status_;
  }
  if (estimate_ >= options_.block_bytes) {
    status_ = Flush();
    if (!status_.ok()) {
      return status_;
    }
  }
  const auto user = UserKey(entry.key);
  if (user_keys_.empty() || user_keys_.back() != user) {
    user_keys_.emplace_back(user);
  }
  pending_.push_back(entry);
  estimate_ += entry.key.size() + entry.value.size() + 15;
  last_key_ = entry.key;
  return {};
}

auto TableWriter::Finish() -> Status {
  if (!status_.ok()) {
    return status_;
  }
  if (finished_) {
    return Status::InvalidArgument("table is already finished");
  }
  status_ = Flush();
  std::string filter;
  std::string index;
  std::string filter_handle;
  std::string index_handle;
  if (status_.ok()) {
    status_ = Bloom::Build(user_keys_, options_.bloom_bits_per_key, filter);
  }
  if (status_.ok()) {
    status_ = Append(filter, filter_handle);
  }
  if (status_.ok()) {
    status_ = EncodeBlock(index_, index);
  }
  if (status_.ok()) {
    status_ = Append(index, index_handle);
  }
  if (status_.ok()) {
    std::string footer = filter_handle + index_handle;
    AppendFixed<4>(footer, kVersion);
    AppendFixed<8>(footer, kMagic);
    Seal(footer);
    status_ = file_.Append(footer);
  }
  if (status_.ok()) {
    status_ = file_.Sync();
  }
  finished_ = status_.ok();
  return status_;
}

auto WriteTable(const std::filesystem::path& path, std::span<const Entry> entries,
                const TableOptions& options) -> Status {
  auto status = Validate(options);
  if (!status.ok()) {
    return status;
  }
  std::error_code error;
  const auto exists = std::filesystem::exists(path, error);
  if (error) {
    return Status::IOError("table destination: " + error.message());
  }
  if (exists) {
    return Status::InvalidArgument("immutable table already exists");
  }
  auto temporary = path;
  temporary += std::filesystem::path(".tmp");
  std::unique_ptr<platform::File> file;
  status = platform::File::Open(temporary, true, file);
  if (!status.ok()) {
    return status;
  }
  status = file->Truncate(0);
  if (status.ok()) {
    TableWriter writer(*file, options);
    for (const auto& entry : entries) {
      status = writer.Add(entry);
      if (!status.ok()) {
        break;
      }
    }
    if (status.ok()) {
      status = writer.Finish();
    }
  }
  file.reset();
  if (status.ok()) {
    status = platform::AtomicRename(temporary, path);
  }
  if (!status.ok()) {
    // A failed POSIX directory sync may mean the rename already happened.
    // Preserve any destination; only remove an unpublished temporary file.
    std::filesystem::remove(temporary, error);
  }
  return status;
}

auto TableReader::Open(const std::filesystem::path& path, std::unique_ptr<TableReader>& output)
    -> Status {
  auto reader = std::make_unique<TableReader>();
  auto status = platform::MappedFile::Open(path, reader->mapping_);
  if (!status.ok()) {
    return status;
  }
  reader->bytes_ = reader->mapping_->Bytes();
  status = reader->Parse();
  if (status.ok()) {
    output = std::move(reader);
  }
  return status;
}

auto TableReader::FromBytes(std::string bytes, std::unique_ptr<TableReader>& output) -> Status {
  auto reader = std::make_unique<TableReader>();
  reader->owned_ = std::move(bytes);
  reader->bytes_ = reader->owned_;
  auto status = reader->Parse();
  if (status.ok()) {
    output = std::move(reader);
  }
  return status;
}

auto TableReader::Parse() -> Status {
  if (bytes_.size() < kFooterSize) {
    return Status::Corruption("short table footer");
  }
  const auto footer = bytes_.substr(bytes_.size() - kFooterSize);
  if (!ChecksumValid(footer) || LoadFixed(footer.substr(32), 4) != kVersion ||
      LoadFixed(footer.substr(36), 8) != kMagic) {
    return Status::Corruption("table footer CRC, version or magic");
  }
  const auto blocks = bytes_.substr(0, bytes_.size() - kFooterSize);
  std::string_view filter;
  std::string_view index;
  if (!Range(blocks, footer.substr(0, 16), filter) ||
      !Range(blocks, footer.substr(16, 16), index)) {
    return Status::Corruption("table metadata range");
  }
  const auto filter_offset = LoadFixed(footer, 8);
  const auto index_offset = LoadFixed(footer.substr(16), 8);
  if (filter_offset + filter.size() != index_offset ||
      index_offset + index.size() != blocks.size()) {
    return Status::Corruption("table metadata layout");
  }
  auto status = Bloom::Open(filter, filter_);
  if (!status.ok()) {
    return status;
  }
  status = DecodeBlock(index, index_);
  if (!status.ok()) {
    return status;
  }
  std::uint64_t expected_offset = 0;
  std::string previous;
  for (const auto& entry : index_) {
    std::string_view data;
    if (!Range(blocks.substr(0, static_cast<std::size_t>(filter_offset)), entry.value, data) ||
        LoadFixed(entry.value, 8) != expected_offset) {
      return Status::Corruption("table data range");
    }
    expected_offset += data.size();
    std::vector<Entry> decoded;
    status = DecodeBlock(data, decoded);
    if (!status.ok()) {
      return status;
    }
    if (decoded.empty() || decoded.back().key != entry.key ||
        (!previous.empty() && Compare(previous, decoded.front().key) >= 0)) {
      return Status::Corruption("table index separator or block order");
    }
    for (const auto& item : decoded) {
      if (((Tag(item.key) & 0xffU) == 0 && !item.value.empty()) ||
          !filter_.MayContain(UserKey(item.key))) {
        return Status::Corruption("table tombstone or filter excludes a stored key");
      }
    }
    previous = decoded.back().key;
  }
  return expected_offset == filter_offset ? Status{} : Status::Corruption("unindexed table data");
}

auto TableReader::ReadBlock(const Entry& index_entry, std::vector<Entry>& output) const -> Status {
  std::string_view block;
  if (!Range(bytes_, index_entry.value, block)) {
    return Status::Corruption("table data range");
  }
  return DecodeBlock(block, output);
}

auto TableReader::Get(std::string_view user_key, std::uint64_t sequence, Entry& output) const
    -> Status {
  if (sequence > kMaxSequence) {
    return Status::InvalidArgument("table read sequence overflow");
  }
  if (!filter_.MayContain(user_key)) {
    return Status::NotFound("table filter");
  }
  const auto key = InternalKey(user_key, sequence, WriteBatch::Type::kPut);
  const auto before = [](std::string_view entry, std::string_view target) {
    return Compare(entry, target) < 0;
  };
  const auto block = std::ranges::lower_bound(index_, key, before, &Entry::key);
  if (block == index_.end()) {
    return Status::NotFound("table index");
  }
  std::vector<Entry> entries;
  auto status = ReadBlock(*block, entries);
  if (!status.ok()) {
    return status;
  }
  const auto entry = std::ranges::lower_bound(entries, key, before, &Entry::key);
  if (entry == entries.end() || UserKey(entry->key) != user_key) {
    return Status::NotFound("table data");
  }
  output = *entry;
  return {};
}

auto TableReader::ReadAll(std::vector<Entry>& output) const -> Status {
  std::vector<Entry> result;
  for (const auto& entry : index_) {
    std::vector<Entry> block;
    auto status = ReadBlock(entry, block);
    if (!status.ok()) {
      return status;
    }
    result.insert(result.end(), std::make_move_iterator(block.begin()),
                  std::make_move_iterator(block.end()));
  }
  output = std::move(result);
  return {};
}
}  // namespace lsm::internal::table
