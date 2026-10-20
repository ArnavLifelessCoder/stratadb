#include "table/table.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <random>
#include <thread>

#include "table/format.hpp"
#include "temp_directory.hpp"
#include "test_support.hpp"

namespace lsm::internal::table {
namespace {
class TableFile final : public platform::File {
 public:
  std::string bytes;
  unsigned calls = 0;
  unsigned fail_call = std::numeric_limits<unsigned>::max();
  unsigned syncs = 0;
  bool fail_sync = false;
  auto ReadAll(std::string& output) -> Status override {
    output = bytes;
    return {};
  }
  auto Append(std::string_view input) -> Status override {
    if (++calls == fail_call) {
      bytes.append(input.substr(0, input.size() / 2));
      return Status::IOError("injected partial append");
    }
    bytes.append(input);
    return {};
  }
  auto Sync() -> Status override {
    ++syncs;
    return fail_sync ? Status::IOError("injected sync failure") : Status{};
  }
  auto Truncate(std::uint64_t) -> Status override { return Status::IOError("unused"); }
  auto Lock() -> Status override { return Status::IOError("unused"); }
};

auto Build(std::span<const Entry> entries, TableFile& file, std::size_t block_bytes = 64)
    -> Status {
  TableWriter writer(file, {.block_bytes = block_bytes});
  for (const auto& entry : entries) {
    auto status = writer.Add(entry);
    if (!status.ok()) {
      return status;
    }
  }
  return writer.Finish();
}

auto SampleEntries() -> std::vector<Entry> {
  std::vector<Entry> entries;
  for (std::uint64_t user = 0; user < 10; ++user) {
    for (std::uint64_t sequence = 3; sequence > 0; --sequence) {
      const auto type = sequence == 2 ? WriteBatch::Type::kDelete : WriteBatch::Type::kPut;
      entries.push_back({.key = InternalKey(std::to_string(user), sequence, type),
                         .value = sequence == 2 ? "" : "v" + std::to_string(sequence)});
    }
  }
  return entries;
}

TEST(TableTest, NativeRoundTripKeyValueMixesAndVersionLookupsMatchReference) {
  SCOPED_TRACE(test::seed);
  std::mt19937_64 random(test::seed);
  test::TempDirectory directory;
  std::vector<Entry> entries;
  for (std::uint64_t user = 0; user < 160; ++user) {
    auto key = user == 0 ? "" : std::string(static_cast<std::size_t>(random() % 300), 'p');
    if (user != 0) {
      key += std::to_string(user);
      key.push_back(static_cast<char>(user));
    }
    for (std::uint64_t sequence = 3; sequence > 0; --sequence) {
      const auto type = sequence == 2 ? WriteBatch::Type::kDelete : WriteBatch::Type::kPut;
      const auto size = sequence == 2 || user % 7 == 0 ? std::size_t{0}
                        : user == 1                    ? std::size_t{1 << 20}
                                                       : static_cast<std::size_t>(random() % 1000);
      entries.push_back({.key = InternalKey(key, sequence, type),
                         .value = std::string(size, static_cast<char>(random()))});
    }
  }
  std::ranges::sort(entries,
                    [](const Entry& a, const Entry& b) { return Compare(a.key, b.key) < 0; });
  for (const auto block_bytes : {1U, 128U, 4096U, 65536U}) {
    for (const int bloom : {0, 10}) {
      const auto path =
          directory.path() / (std::to_string(block_bytes) + "-" + std::to_string(bloom) + ".sst");
      ASSERT_TRUE(
          WriteTable(path, entries, {.block_bytes = block_bytes, .bloom_bits_per_key = bloom})
              .ok());
      EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(path).concat(".tmp")));
      std::unique_ptr<TableReader> reader;
      ASSERT_TRUE(TableReader::Open(path, reader).ok());
      std::vector<Entry> actual;
      ASSERT_TRUE(reader->ReadAll(actual).ok());
      EXPECT_EQ(actual, entries);
      for (std::size_t i = 0; i < entries.size(); i += 3) {
        for (std::uint64_t sequence = 0; sequence <= 4; ++sequence) {
          Entry value{.key = "sentinel", .value = "sentinel"};
          const auto status = reader->Get(UserKey(entries[i].key), sequence, value);
          if (sequence == 0) {
            EXPECT_TRUE(status.IsNotFound());
            EXPECT_EQ(value.key, "sentinel");
          } else {
            ASSERT_TRUE(status.ok());
            EXPECT_EQ(value, entries[i + (sequence >= 3 ? 0 : 3 - sequence)]);
          }
        }
      }
      Entry missing;
      EXPECT_TRUE(reader->Get("missing", kMaxSequence, missing).IsNotFound());
      EXPECT_EQ(reader->Get("", kMaxSequence + 1, missing).code(), StatusCode::kInvalidArgument);
    }
  }
}

TEST(TableTest, EmptyTableAndImmutableDestinationAndInvalidOptions) {
  test::TempDirectory directory;
  const auto path = directory.path() / "empty.sst";
  EXPECT_EQ(WriteTable(path, {}, {.block_bytes = 0}).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(WriteTable(path, {}, {.bloom_bits_per_key = -1}).code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(std::filesystem::exists(path));
  ASSERT_TRUE(WriteTable(path, {}).ok());
  EXPECT_EQ(WriteTable(path, SampleEntries()).code(), StatusCode::kInvalidArgument);
  std::unique_ptr<TableReader> reader;
  ASSERT_TRUE(TableReader::Open(path, reader).ok());
  std::vector<Entry> entries;
  ASSERT_TRUE(reader->ReadAll(entries).ok());
  EXPECT_TRUE(entries.empty());
  Entry value;
  EXPECT_TRUE(reader->Get("", 1, value).IsNotFound());
  const auto* previous = reader.get();
  EXPECT_EQ(TableReader::Open(directory.path() / "absent", reader).code(), StatusCode::kIOError);
  EXPECT_EQ(reader.get(), previous);
  EXPECT_EQ(WriteTable(directory.path() / "missing" / "file", {}).code(), StatusCode::kIOError);
  auto unsorted = SampleEntries();
  std::swap(unsorted[0], unsorted[1]);
  const auto invalid = directory.path() / "invalid.sst";
  EXPECT_EQ(WriteTable(invalid, unsorted).code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(std::filesystem::exists(invalid));
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(invalid).concat(".tmp")));
}

TEST(TableTest, EveryByteFlipAndEveryTruncationRejectsDataFilterIndexAndFooter) {
  const auto entries = SampleEntries();
  TableFile file;
  ASSERT_TRUE(Build(entries, file).ok());
  std::unique_ptr<TableReader> reader;
  ASSERT_TRUE(TableReader::FromBytes(file.bytes, reader).ok());
  const auto* original = reader.get();
  for (std::size_t i = 0; i < file.bytes.size(); ++i) {
    SCOPED_TRACE(i);
    auto damaged = file.bytes;
    damaged[i] ^= 1;
    EXPECT_EQ(TableReader::FromBytes(std::move(damaged), reader).code(), StatusCode::kCorruption);
    EXPECT_EQ(reader.get(), original);
    EXPECT_EQ(TableReader::FromBytes(file.bytes.substr(0, i), reader).code(),
              StatusCode::kCorruption);
    EXPECT_EQ(reader.get(), original);
  }
}

TEST(TableTest, ForgedFooterHandlesAndIndexWithValidChecksumsAreRejected) {
  TableFile file;
  ASSERT_TRUE(Build(SampleEntries(), file).ok());
  for (const std::size_t offset : {0U, 8U, 16U, 24U, 32U, 36U}) {
    auto damaged = file.bytes.substr(0, file.bytes.size() - 4);
    damaged[file.bytes.size() - 48 + offset] ^= 0x40;
    const auto crc = Crc32c(std::string_view(damaged).substr(damaged.size() - 44));
    AppendFixed<4>(damaged, crc);
    std::unique_ptr<TableReader> reader;
    EXPECT_EQ(TableReader::FromBytes(std::move(damaged), reader).code(), StatusCode::kCorruption);
  }
  const auto footer = std::string_view(file.bytes).substr(file.bytes.size() - 48);
  const auto index_offset = static_cast<std::size_t>(LoadFixed(footer.substr(16), 8));
  const auto index_size = static_cast<std::size_t>(LoadFixed(footer.substr(24), 8));
  std::vector<Entry> index;
  ASSERT_TRUE(
      DecodeBlock(std::string_view(file.bytes).substr(index_offset, index_size), index).ok());
  index.front().value.assign(16, '\xff');
  std::string encoded;
  ASSERT_TRUE(EncodeBlock(index, encoded).ok());
  ASSERT_EQ(encoded.size(), index_size);
  auto damaged = file.bytes;
  damaged.replace(index_offset, index_size, encoded);
  std::unique_ptr<TableReader> reader;
  EXPECT_EQ(TableReader::FromBytes(std::move(damaged), reader).code(), StatusCode::kCorruption);
}

TEST(TableTest, PartialWritesAndSyncFailureAreStickyAndNeverFinishSuccessfully) {
  const auto entries = SampleEntries();
  TableFile complete;
  ASSERT_TRUE(Build(entries, complete).ok());
  EXPECT_EQ(complete.syncs, 1U);
  for (unsigned fail = 1; fail <= complete.calls + 1; ++fail) {
    TableFile file;
    file.fail_call = fail;
    file.fail_sync = fail == complete.calls + 1;
    TableWriter writer(file, {.block_bytes = 64});
    Status status;
    for (const auto& entry : entries) {
      status = writer.Add(entry);
      if (!status.ok()) {
        break;
      }
    }
    if (status.ok()) {
      status = writer.Finish();
    }
    EXPECT_EQ(status.code(), StatusCode::kIOError);
    const auto calls = file.calls;
    EXPECT_EQ(writer.Finish().code(), StatusCode::kIOError);
    EXPECT_EQ(writer.Add(entries.back()).code(), StatusCode::kIOError);
    EXPECT_EQ(file.calls, calls);
    EXPECT_EQ(file.syncs, file.fail_sync ? 1U : 0U);
  }
  TableFile file;
  TableWriter writer(file, {});
  ASSERT_TRUE(writer.Finish().ok());
  EXPECT_EQ(writer.Add(entries.front()).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(writer.Finish().code(), StatusCode::kInvalidArgument);
}

TEST(TableTest, ConcurrentMappedReadsAcrossBlocksMatchKnownVersions) {
  test::TempDirectory directory;
  const auto path = directory.path() / "concurrent.sst";
  const auto entries = SampleEntries();
  ASSERT_TRUE(WriteTable(path, entries, {.block_bytes = 1}).ok());
  std::unique_ptr<TableReader> reader;
  ASSERT_TRUE(TableReader::Open(path, reader).ok());
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> threads;
  for (unsigned thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&, thread] {
      std::mt19937_64 random(test::seed + thread);
      for (unsigned i = 0; i < 10000; ++i) {
        const auto user = random() % 10;
        const auto sequence = 1 + random() % 3;
        Entry entry;
        if (!reader->Get(std::to_string(user), sequence, entry).ok() ||
            entry != entries[static_cast<std::size_t>(user * 3 + 3 - sequence)]) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0U);
}
}  // namespace
}  // namespace lsm::internal::table
