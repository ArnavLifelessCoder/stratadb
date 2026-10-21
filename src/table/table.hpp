#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "platform/file.hpp"
#include "platform/mapping.hpp"
#include "table/block.hpp"
#include "table/block_cache.hpp"
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
  // Counts data-block reads from the file (including CRC checks); block-cache
  // hits and metadata-only opens are not counted. Diagnostic counter shared by
  // readers; tests compare quiescent intervals.
  static auto DataBlocksRead() -> std::uint64_t { return data_blocks_read_.load(); }
  // With a cache, data blocks are CRC-checked once when copied into it and
  // served from it afterwards. file_number keys this table's blocks.
  static auto Open(const std::filesystem::path& path, std::unique_ptr<TableReader>& output,
                   std::shared_ptr<BlockCache> cache = nullptr, std::uint64_t file_number = 0)
      -> Status;
  // Owned bytes use the same parser as mapped files; useful for format tools and tests.
  static auto FromBytes(std::string bytes, std::unique_ptr<TableReader>& output) -> Status;
  // Returns the newest entry <= sequence, including tombstones; NotFound means
  // no visible version. Outputs remain unchanged on error. Safe for concurrent reads.
  auto Get(std::string_view user_key, std::uint64_t sequence, Entry& output) const -> Status;
  auto ReadAll(std::vector<Entry>& output) const -> Status;
  // Explicit full scan for integrity tools; Open checks metadata only.
  auto Verify() const -> Status;

 private:
  friend class TableIterator;
  // One data block. `owner` keeps cached bytes alive; without a cache the
  // bytes borrow the mapping and `verified` is false, so the block parser
  // checks the CRC itself.
  struct BlockRef {
    BlockCache::Block owner;
    std::string_view bytes;
    bool verified = false;
  };
  inline static std::atomic<std::uint64_t> data_blocks_read_{0};
  auto Parse() -> Status;
  auto ReadBlock(const Entry& index_entry, std::vector<Entry>& output) const -> Status;
  auto LoadBlock(std::string_view handle, BlockRef& output) const -> Status;
  std::shared_ptr<BlockCache> cache_;
  std::uint64_t file_number_ = 0;
  std::unique_ptr<platform::MappedFile> mapping_;
  std::string owned_;
  std::string_view bytes_;
  Bloom filter_;
  std::vector<Entry> index_;
};
}  // namespace lsm::internal::table
