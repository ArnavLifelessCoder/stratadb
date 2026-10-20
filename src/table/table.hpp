#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "platform/file.hpp"
#include "platform/mapping.hpp"
#include "table/block.hpp"
#include "table/bloom.hpp"

namespace lsm::internal::table {
struct TableOptions {
  std::size_t block_bytes = 4096;
  int bloom_bits_per_key = 10;
};

// Streaming builder over a new, empty file. Finish writes the footer and syncs.
// Caller owns the file and publishes it only after Finish succeeds and it is closed.
class TableWriter {
 public:
  TableWriter(platform::File& file, TableOptions options);
  auto Add(const Entry& entry) -> Status;
  auto Finish() -> Status;

 private:
  auto Flush() -> Status;
  auto Append(std::string_view bytes, std::string& handle) -> Status;
  platform::File& file_;
  TableOptions options_;
  Status status_;
  bool finished_ = false;
  std::uint64_t offset_ = 0;
  std::size_t estimate_ = 0;
  std::string last_key_;
  std::vector<Entry> pending_;
  std::vector<Entry> index_;
  std::vector<std::string> user_keys_;
};

// Path and its ".tmp" sibling must be exclusively owned by the caller.
// Rejects an existing destination. Syncs the temp file, closes it, then publishes
// with the platform's atomic rename and directory durability protocol.
auto WriteTable(const std::filesystem::path& path, std::span<const Entry> entries,
                const TableOptions& options = {}) -> Status;

class TableReader {
 public:
  static auto Open(const std::filesystem::path& path, std::unique_ptr<TableReader>& output)
      -> Status;
  // Owned bytes use the same parser as mapped files; useful for format tools and tests.
  static auto FromBytes(std::string bytes, std::unique_ptr<TableReader>& output) -> Status;
  // Returns the newest entry <= sequence, including tombstones; NotFound means
  // no visible version. Outputs remain unchanged on error. Safe for concurrent reads.
  auto Get(std::string_view user_key, std::uint64_t sequence, Entry& output) const -> Status;
  auto ReadAll(std::vector<Entry>& output) const -> Status;

 private:
  auto Parse() -> Status;
  auto ReadBlock(const Entry& index_entry, std::vector<Entry>& output) const -> Status;
  std::unique_ptr<platform::MappedFile> mapping_;
  std::string owned_;
  std::string_view bytes_;
  Bloom filter_;
  std::vector<Entry> index_;
};
}  // namespace lsm::internal::table
