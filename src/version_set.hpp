#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "platform/file.hpp"
#include "table/table.hpp"

namespace lsm::internal {
struct TableMetadata {
  std::uint64_t number = 0;
  std::uint32_t level = 0;
  std::uint64_t size = 0;
  std::string smallest;
  std::string largest;
  std::shared_ptr<const table::TableReader> reader;
};

struct Version {
  std::vector<TableMetadata> files;
};

struct VersionEdit {
  std::uint64_t next_file = 3;
  std::uint64_t last_sequence = 0;
  std::uint64_t flushed_sequence = 0;
  std::uint64_t current_wal = 2;
  std::uint64_t previous_wal = 0;
  std::uint64_t pending_table = 0;
  std::vector<TableMetadata> added;
  std::vector<std::uint64_t> removed;
};

struct ManifestState {
  // Metadata fields describe the last durable edit, not unsynced current writes.
  VersionEdit metadata;
  std::shared_ptr<const Version> version = std::make_shared<Version>();
};

auto WalPath(const std::filesystem::path& directory, std::uint64_t number) -> std::filesystem::path;
auto TablePath(const std::filesystem::path& directory, std::uint64_t number)
    -> std::filesystem::path;
auto WriteAtomicFile(const std::filesystem::path& path, std::string_view bytes) -> Status;
auto EncodeEdit(const VersionEdit& edit, std::string& output) -> Status;
auto DecodeEdit(std::string_view input, VersionEdit& output) -> Status;

// Externally serialized. Versions and mapped readers remain alive through shared
// ownership after replacement; no live table is deleted by this component.
class VersionSet {
 public:
  static auto Open(const std::filesystem::path& directory, std::unique_ptr<VersionSet>& output)
      -> Status;
  // File boundary is injectable for append/sync/recovery fault tests.
  static auto Recover(const std::filesystem::path& directory, std::unique_ptr<platform::File> log,
                      std::unique_ptr<VersionSet>& output) -> Status;
  auto Apply(const VersionEdit& edit) -> Status;
  [[nodiscard]] auto State() const -> const ManifestState& { return state_; }
  [[nodiscard]] auto BytesWritten() const -> std::uint64_t { return bytes_written_; }

 private:
  auto LoadReaders(ManifestState& state) -> Status;
  std::filesystem::path directory_;
  std::unique_ptr<platform::File> log_;
  ManifestState state_;
  Status failure_;
  std::uint64_t bytes_written_ = 0;
};
}  // namespace lsm::internal
