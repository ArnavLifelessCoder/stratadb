#include "version_set.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <set>

#include "table/format.hpp"
#include "util/varint.hpp"

namespace lsm::internal {
namespace {
constexpr std::string_view kManifestName = "MANIFEST-1";
constexpr std::size_t kHeader = 9;

auto Inspect(const std::filesystem::path& path, bool& exists) -> Status {
  std::error_code error;
  exists = std::filesystem::exists(path, error);
  return error ? Status::IOError("inspect file: " + error.message()) : Status{};
}

void AppendString(std::string& output, std::string_view value) {
  AppendVarint64(output, value.size());
  output.append(value);
}

auto ReadString(std::string_view& input, std::string& output) -> bool {
  std::uint64_t size = 0;
  if (!ConsumeVarint64(input, size) || size > input.size()) {
    return false;
  }
  output = input.substr(0, static_cast<std::size_t>(size));
  input.remove_prefix(static_cast<std::size_t>(size));
  return true;
}

auto ApplyTo(const ManifestState& current, const VersionEdit& edit, ManifestState& output)
    -> Status {
  if (edit.next_file < current.metadata.next_file || edit.current_wal < 2 ||
      edit.current_wal >= edit.next_file || edit.last_sequence > kMaxSequence ||
      edit.last_sequence < current.metadata.last_sequence ||
      edit.flushed_sequence < current.metadata.flushed_sequence ||
      edit.flushed_sequence > edit.last_sequence ||
      ((edit.previous_wal == 0) != (edit.pending_table == 0)) ||
      (edit.previous_wal != 0 &&
       (edit.previous_wal < 2 || edit.previous_wal >= edit.next_file || edit.pending_table < 2 ||
        edit.pending_table >= edit.next_file || edit.previous_wal == edit.current_wal ||
        edit.pending_table == edit.current_wal || edit.pending_table == edit.previous_wal))) {
    return Status::Corruption("manifest counters or WAL state");
  }
  auto version = std::make_shared<Version>(*current.version);
  for (const auto number : edit.removed) {
    const auto found = std::ranges::find(version->files, number, &TableMetadata::number);
    if (found == version->files.end()) {
      return Status::Corruption("manifest removes an unknown table");
    }
    version->files.erase(found);
  }
  version->files.insert(version->files.end(), edit.added.begin(), edit.added.end());
  std::set<std::uint64_t> numbers;
  for (const auto& file : version->files) {
    if (file.number < 2 || file.number >= edit.next_file || file.level > 6 || file.size == 0 ||
        file.smallest > file.largest || file.number == edit.current_wal ||
        file.number == edit.previous_wal || file.number == edit.pending_table ||
        !numbers.insert(file.number).second) {
      return Status::Corruption("manifest table metadata");
    }
  }
  std::ranges::sort(version->files, {}, &TableMetadata::number);
  for (auto& level : version->levels) {
    level.clear();
  }
  for (std::size_t index = 0; index < version->files.size(); ++index) {
    version->levels[version->files[index].level].push_back(index);
  }
  for (std::size_t level = 1; level < version->levels.size(); ++level) {
    auto& indices = version->levels[level];
    std::ranges::sort(indices, [&](auto left, auto right) {
      return version->files[left].smallest < version->files[right].smallest;
    });
    for (std::size_t index = 1; index < indices.size(); ++index) {
      if (version->files[indices[index - 1]].largest >= version->files[indices[index]].smallest) {
        return Status::Corruption("overlapping tables above L0");
      }
    }
  }
  output.metadata = edit;
  output.metadata.added.clear();
  output.metadata.removed.clear();
  output.version = std::move(version);
  return {};
}

auto Frame(const VersionEdit& edit, std::string& output) -> Status {
  std::string payload;
  auto status = EncodeEdit(edit, payload);
  if (!status.ok()) {
    return status;
  }
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("manifest record too large");
  }
  std::string body;
  table::AppendFixed<4>(body, payload.size());
  body.push_back('\1');
  body.append(payload);
  output.clear();
  table::AppendFixed<4>(output, Crc32c(body));
  output.append(body);
  return {};
}

auto LaterRecord(std::string_view bytes) -> bool {
  for (std::size_t i = 1; i + kHeader <= bytes.size(); ++i) {
    const auto candidate = bytes.substr(i);
    const auto length = table::LoadFixed(candidate.substr(4), 4);
    if (candidate[8] == '\1' && length <= candidate.size() - kHeader &&
        table::LoadFixed(candidate, 4) ==
            Crc32c(candidate.substr(4, 5 + static_cast<std::size_t>(length)))) {
      VersionEdit edit;
      if (DecodeEdit(candidate.substr(kHeader, static_cast<std::size_t>(length)), edit).ok()) {
        return true;
      }
    }
  }
  return false;
}
}  // namespace

auto WalPath(const std::filesystem::path& directory, std::uint64_t number)
    -> std::filesystem::path {
  return directory / (std::to_string(number) + ".wal");
}
auto TablePath(const std::filesystem::path& directory, std::uint64_t number)
    -> std::filesystem::path {
  return directory / (std::to_string(number) + ".sst");
}

auto WriteAtomicFile(const std::filesystem::path& path, std::string_view bytes) -> Status {
  auto temporary = path;
  temporary += std::filesystem::path(".tmp");
  std::unique_ptr<platform::File> file;
  auto status = platform::File::Open(temporary, true, file);
  if (status.ok()) {
    status = file->Truncate(0);
  }
  if (status.ok()) {
    status = file->Append(bytes);
  }
  if (status.ok()) {
    status = file->Sync();
  }
  file.reset();
  return status.ok() ? platform::AtomicRename(temporary, path) : status;
}

auto EncodeEdit(const VersionEdit& edit, std::string& output) -> Status {
  std::string bytes;
  AppendVarint32(bytes, 1);
  for (const auto value : {edit.next_file, edit.last_sequence, edit.flushed_sequence,
                           edit.current_wal, edit.previous_wal, edit.pending_table}) {
    AppendVarint64(bytes, value);
  }
  AppendVarint64(bytes, edit.added.size());
  for (const auto& file : edit.added) {
    AppendVarint64(bytes, file.number);
    AppendVarint32(bytes, file.level);
    AppendVarint64(bytes, file.size);
    AppendString(bytes, file.smallest);
    AppendString(bytes, file.largest);
  }
  AppendVarint64(bytes, edit.removed.size());
  for (const auto number : edit.removed) {
    AppendVarint64(bytes, number);
  }
  output = std::move(bytes);
  return {};
}

auto DecodeEdit(std::string_view input, VersionEdit& output) -> Status {
  VersionEdit edit;
  std::uint32_t format = 0;
  if (!ConsumeVarint32(input, format) || format != 1 || !ConsumeVarint64(input, edit.next_file) ||
      !ConsumeVarint64(input, edit.last_sequence) ||
      !ConsumeVarint64(input, edit.flushed_sequence) || !ConsumeVarint64(input, edit.current_wal) ||
      !ConsumeVarint64(input, edit.previous_wal) || !ConsumeVarint64(input, edit.pending_table)) {
    return Status::Corruption("manifest edit header");
  }
  std::uint64_t count = 0;
  if (!ConsumeVarint64(input, count) || count > input.size() / 5) {
    return Status::Corruption("manifest added count");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    TableMetadata file;
    if (!ConsumeVarint64(input, file.number) || !ConsumeVarint32(input, file.level) ||
        !ConsumeVarint64(input, file.size) || !ReadString(input, file.smallest) ||
        !ReadString(input, file.largest)) {
      return Status::Corruption("manifest table entry");
    }
    edit.added.push_back(std::move(file));
  }
  if (!ConsumeVarint64(input, count) || count > input.size()) {
    return Status::Corruption("manifest removed count");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t number = 0;
    if (!ConsumeVarint64(input, number)) {
      return Status::Corruption("manifest removed entry");
    }
    edit.removed.push_back(number);
  }
  if (!input.empty()) {
    return Status::Corruption("manifest trailing payload");
  }
  output = std::move(edit);
  return {};
}

auto VersionSet::LoadReaders(ManifestState& state) -> Status {
  auto version = std::make_shared<Version>(*state.version);
  for (auto& file : version->files) {
    if (file.reader) {
      continue;
    }
    const auto path = TablePath(directory_, file.number);
    bool exists = false;
    auto status = Inspect(path, exists);
    if (!status.ok()) {
      return status;
    }
    if (!exists) {
      return Status::Corruption("manifest references a missing table");
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
      return Status::IOError("table size: " + error.message());
    }
    if (size != file.size) {
      return Status::Corruption("manifest table size mismatch");
    }
    std::unique_ptr<table::TableReader> reader;
    status = table::TableReader::Open(path, reader);
    if (!status.ok()) {
      return status;
    }
    file.reader = std::move(reader);
  }
  state.version = std::move(version);
  return {};
}

auto VersionSet::Recover(const std::filesystem::path& directory,
                         std::unique_ptr<platform::File> log, std::unique_ptr<VersionSet>& output)
    -> Status {
  if (!log) {
    return Status::InvalidArgument("null manifest file");
  }
  auto versions = std::make_unique<VersionSet>();
  versions->directory_ = directory;
  versions->log_ = std::move(log);
  std::string bytes;
  auto status = versions->log_->ReadAll(bytes);
  if (!status.ok()) {
    return status;
  }
  std::string_view remaining(bytes);
  std::size_t records = 0;
  while (!remaining.empty()) {
    bool torn =
        remaining.size() < kHeader || remaining.find_first_not_of('\0') == std::string_view::npos;
    std::size_t length = 0;
    if (!torn) {
      length = static_cast<std::size_t>(table::LoadFixed(remaining.substr(4), 4));
      torn = length > remaining.size() - kHeader;
      if (!torn && table::LoadFixed(remaining, 4) != Crc32c(remaining.substr(4, 5 + length))) {
        if (length + kHeader != remaining.size()) {
          return Status::Corruption("manifest middle CRC mismatch");
        }
        torn = true;
      }
    }
    if (torn) {
      if (records == 0 || LaterRecord(remaining)) {
        return Status::Corruption("manifest initial or middle damage");
      }
      const auto offset = bytes.size() - remaining.size();
      status = versions->log_->Truncate(offset);
      if (status.ok()) {
        status = versions->log_->Sync();
      }
      if (!status.ok()) {
        return status;
      }
      std::clog << "Manifest recovery: truncated " << remaining.size() << " tail bytes\n";
      break;
    }
    if (remaining[8] != '\1') {
      return Status::Corruption("manifest record type");
    }
    VersionEdit edit;
    status = DecodeEdit(remaining.substr(kHeader, length), edit);
    if (!status.ok()) {
      return status;
    }
    ManifestState next;
    status = ApplyTo(versions->state_, edit, next);
    if (!status.ok()) {
      return status;
    }
    versions->state_ = std::move(next);
    remaining.remove_prefix(kHeader + length);
    ++records;
  }
  if (records == 0) {
    return Status::Corruption("empty manifest");
  }
  status = versions->LoadReaders(versions->state_);
  if (!status.ok()) {
    return status;
  }
  versions->published_.store(std::make_shared<ManifestState>(versions->state_),
                             std::memory_order_release);
  output = std::move(versions);
  return {};
}

auto VersionSet::Open(const std::filesystem::path& directory, std::unique_ptr<VersionSet>& output)
    -> Status {
  const auto manifest = directory / kManifestName;
  const auto current = directory / "CURRENT";
  bool have_current = false;
  bool have_manifest = false;
  std::uint64_t bootstrap_bytes = 0;
  auto status = Inspect(current, have_current);
  if (status.ok()) {
    status = Inspect(manifest, have_manifest);
  }
  if (!status.ok()) {
    return status;
  }
  if (have_current) {
    std::unique_ptr<platform::File> file;
    status = platform::File::Open(current, false, file);
    std::string name;
    if (status.ok()) {
      status = file->ReadAll(name);
    }
    if (!status.ok()) {
      return status;
    }
    if (name != std::string(kManifestName) + "\n" || !have_manifest) {
      return Status::Corruption("CURRENT does not name the manifest");
    }
  }
  if (!have_manifest) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; it != end && !error;
         it.increment(error)) {
      const auto& path = it->path();
      if (path.extension() == ".sst" ||
          (path.extension() == ".wal" && path.filename() != WalPath({}, 2))) {
        return Status::Corruption("missing manifest for an existing database");
      }
    }
    if (error) {
      return Status::IOError("inspect database directory: " + error.message());
    }
    std::string legacy_bytes;
    bool have_wal = false;
    status = Inspect(WalPath(directory, 2), have_wal);
    if (!status.ok()) {
      return status;
    }
    bool legacy = false;
    status = Inspect(directory / "WAL", legacy);
    if (!status.ok()) {
      return status;
    }
    if (!have_wal && legacy) {
      std::unique_ptr<platform::File> file;
      status = platform::File::Open(directory / "WAL", false, file);
      if (status.ok()) {
        status = file->ReadAll(legacy_bytes);
      }
    }
    if (status.ok() && !have_wal) {
      status = WriteAtomicFile(WalPath(directory, 2), legacy_bytes);
      if (status.ok()) {
        bootstrap_bytes += legacy_bytes.size();
      }
    }
    std::string initial;
    if (status.ok()) {
      status = Frame({}, initial);
    }
    if (status.ok()) {
      status = WriteAtomicFile(manifest, initial);
    }
    if (!status.ok()) {
      return status;
    }
    bootstrap_bytes += initial.size();
  }
  std::unique_ptr<platform::File> file;
  status = platform::File::Open(manifest, false, file);
  std::unique_ptr<VersionSet> versions;
  if (status.ok()) {
    status = Recover(directory, std::move(file), versions);
  }
  if (!status.ok()) {
    return status;
  }
  // Finish an interrupted initial publication using the durable manifest.
  if (!have_current) {
    status = WriteAtomicFile(current, std::string(kManifestName) + "\n");
    if (!status.ok()) {
      return status;
    }
    bootstrap_bytes += kManifestName.size() + 1;
  }
  versions->bytes_written_ += bootstrap_bytes;
  output = std::move(versions);
  return {};
}

auto VersionSet::Apply(const VersionEdit& edit) -> Status {
  if (!failure_.ok()) {
    return failure_;
  }
  ManifestState next;
  auto status = ApplyTo(state_, edit, next);
  if (!status.ok()) {
    return Status::InvalidArgument(std::string(status.message()));
  }
  status = LoadReaders(next);
  std::string record;
  if (status.ok()) {
    status = Frame(edit, record);
  }
  if (!status.ok()) {
    return status;
  }
  failure_ = log_->Append(record);
  if (failure_.ok()) {
    bytes_written_ += record.size();
    failure_ = log_->Sync();
  }
  if (failure_.ok()) {
    state_ = std::move(next);
    published_.store(std::make_shared<ManifestState>(state_), std::memory_order_release);
  }
  return failure_;
}
}  // namespace lsm::internal
