#pragma once

#include <array>
#include <atomic>
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
  // Indices into files: L0 by file number, higher levels by smallest user key.
  std::array<std::vector<std::size_t>, 7> levels;
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

// Apply calls are externally serialized. State/BytesWritten may run concurrently
// with Apply, including its file I/O. Published states are immutable.
class VersionSet {
 public:
  static auto Open(const std::filesystem::path& directory, std::unique_ptr<VersionSet>& output)
      -> Status;
  // File boundary is injectable for append/sync/recovery fault tests.
  static auto Recover(const std::filesystem::path& directory, std::unique_ptr<platform::File> log,
                      std::unique_ptr<VersionSet>& output) -> Status;
  auto Apply(const VersionEdit& edit) -> Status;
  [[nodiscard]] auto State() const -> ManifestState {
    return *published_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto BytesWritten() const -> std::uint64_t { return bytes_written_.load(); }

 private:
  auto LoadReaders(ManifestState& state) -> Status;
  std::filesystem::path directory_;
  std::unique_ptr<platform::File> log_;
  ManifestState state_;
  std::atomic<std::shared_ptr<const ManifestState>> published_{std::make_shared<ManifestState>()};
  Status failure_;
  std::atomic<std::uint64_t> bytes_written_{0};
};
}  // namespace lsm::internal
