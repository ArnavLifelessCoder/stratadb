#include "wal.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <memory>

#include "util/crc32c.hpp"

namespace lsm::internal {
namespace {
class MemoryFile final : public platform::File {
 public:
  std::string bytes;
  bool fail_sync = false;
  bool fail_truncate = false;
  bool fail_read = false;
  std::size_t append_limit = std::numeric_limits<std::size_t>::max();
  unsigned syncs = 0;
  unsigned appends = 0;
  Status ReadAll(std::string& output) override {
    if (fail_read) {
      return Status::IOError("injected read failure");
    }
    output = bytes;
    return {};
  }
  Status Append(std::string_view input) override {
    ++appends;
    bytes.append(input.substr(0, append_limit));
    return input.size() > append_limit ? Status::IOError("injected short append") : Status{};
  }
  Status Sync() override {
    ++syncs;
    return fail_sync ? Status::IOError("injected sync failure") : Status{};
  }
  Status Truncate(std::uint64_t size) override {
    if (fail_truncate) {
      return Status::IOError("injected truncate failure");
    }
    bytes.resize(static_cast<std::size_t>(size));
    return {};
  }
  Status Lock() override { return {}; }
};

std::string Record(std::uint64_t sequence, std::string_view value = "value") {
  auto file = std::make_unique<MemoryFile>();
  auto* memory = file.get();
  Wal wal(std::move(file));
  WriteBatch batch;
  EXPECT_TRUE(batch.Put("key", value).ok());
  EXPECT_TRUE(wal.Append(batch, sequence, false).ok());
  return memory->bytes;
}

TEST(WalTest, EveryFinalRecordTruncationAndBadCrcRepairsThenAllowsAppend) {
  const auto prefix = Record(1, "old");
  const auto tail = Record(2, "new");
  for (std::size_t length = 0; length < tail.size(); ++length) {
    SCOPED_TRACE(length);
    auto file = std::make_unique<MemoryFile>();
    auto* memory = file.get();
    memory->bytes = prefix + tail.substr(0, length);
    Wal wal(std::move(file));
    MemTable table;
    std::uint64_t last = 0;
    ASSERT_TRUE(wal.Replay(table, last).ok());
    EXPECT_EQ(last, 1U);
    EXPECT_EQ(memory->bytes, prefix);
    EXPECT_EQ(memory->syncs, length == 0 ? 0U : 1U);
    std::string value;
    ASSERT_TRUE(table.Get("key", last, value).ok());
    EXPECT_EQ(value, "old");
    WriteBatch batch;
    ASSERT_TRUE(batch.Delete("key").ok());
    ASSERT_TRUE(wal.Append(batch, 2, true).ok());
    MemTable replayed;
    ASSERT_TRUE(wal.Replay(replayed, last).ok());
    EXPECT_EQ(last, 2U);
    EXPECT_TRUE(replayed.Get("key", last, value).IsNotFound());
  }
  auto file = std::make_unique<MemoryFile>();
  auto* memory = file.get();
  memory->bytes = prefix + tail;
  memory->bytes.back() ^= 1;
  Wal wal(std::move(file));
  MemTable table;
  std::uint64_t last = 0;
  ASSERT_TRUE(wal.Replay(table, last).ok());
  EXPECT_EQ(memory->bytes, prefix);
  EXPECT_EQ(wal.TruncatedBytes(), tail.size());
}

TEST(WalTest, MiddleCorruptionNeverTruncatesEvenWhenLengthSwallowsLaterRecord) {
  const auto first = Record(1);
  const auto valid = first + Record(2);
  for (std::size_t byte = 0; byte < first.size(); ++byte) {
    SCOPED_TRACE(byte);
    auto file = std::make_unique<MemoryFile>();
    auto* memory = file.get();
    memory->bytes = valid;
    memory->bytes[byte] ^= 0x40;
    const auto corrupted = memory->bytes;
    Wal wal(std::move(file));
    MemTable table;
    std::uint64_t last = 0;
    EXPECT_EQ(wal.Replay(table, last).code(), StatusCode::kCorruption);
    EXPECT_EQ(memory->bytes, corrupted);
    EXPECT_EQ(memory->syncs, 0U);
  }
}

TEST(WalTest, ZeroFilledTailIsTruncatedAndSyncedButZeroFilledMiddleIsCorruption) {
  const auto prefix = Record(1);
  for (const std::size_t size : {1U, 9U, 512U, 4096U}) {
    for (const bool middle : {false, true}) {
      auto file = std::make_unique<MemoryFile>();
      auto* memory = file.get();
      memory->bytes = prefix + std::string(size, '\0') + (middle ? Record(2) : "");
      const auto original = memory->bytes;
      Wal wal(std::move(file));
      MemTable table;
      std::uint64_t last = 0;
      const auto status = wal.Replay(table, last);
      if (middle) {
        EXPECT_EQ(status.code(), StatusCode::kCorruption);
        EXPECT_EQ(memory->bytes, original);
        EXPECT_EQ(memory->syncs, 0U);
      } else {
        ASSERT_TRUE(status.ok());
        EXPECT_EQ(last, 1U);
        EXPECT_EQ(memory->bytes, prefix);
        EXPECT_EQ(memory->syncs, 1U);
        EXPECT_EQ(wal.TruncatedBytes(), size);
      }
    }
  }
}

TEST(WalTest, RejectsSequenceGapsAndValidCrcInvalidBatch) {
  for (const bool gap : {false, true}) {
    auto file = std::make_unique<MemoryFile>();
    file->bytes = Record(gap ? 2 : 1);
    if (!gap) {
      file->bytes[11] = 99;  // Invalid operation; recompute CRC to isolate the decoder.
      const auto crc = Crc32c(std::string_view(file->bytes).substr(4));
      for (unsigned i = 0; i < 4; ++i) {
        file->bytes[i] = static_cast<char>((crc >> (8 * i)) & 0xffU);
      }
    }
    Wal wal(std::move(file));
    MemTable table;
    std::uint64_t last = 0;
    EXPECT_EQ(wal.Replay(table, last).code(), StatusCode::kCorruption);
  }
}

TEST(WalTest, SyncAndPartialWriteFailuresPoisonFurtherAppends) {
  for (const bool fail_sync : {false, true}) {
    auto file = std::make_unique<MemoryFile>();
    auto* memory = file.get();
    memory->fail_sync = fail_sync;
    memory->append_limit = fail_sync ? 1000 : 5;
    Wal wal(std::move(file));
    WriteBatch batch;
    ASSERT_TRUE(batch.Put("key", "value").ok());
    EXPECT_EQ(wal.Append(batch, 1, true).code(), StatusCode::kIOError);
    EXPECT_EQ(wal.Append(batch, 2, false).code(), StatusCode::kIOError);
    EXPECT_EQ(memory->appends, 1U);
    EXPECT_EQ(wal.SyncCount(), 0U);
  }
}

TEST(WalTest, ReadTruncateAndRepairSyncFailuresPropagate) {
  for (int mode = 0; mode < 3; ++mode) {
    auto file = std::make_unique<MemoryFile>();
    file->bytes = Record(1) + "torn";
    file->fail_read = mode == 0;
    file->fail_truncate = mode == 1;
    file->fail_sync = mode == 2;
    Wal wal(std::move(file));
    MemTable table;
    std::uint64_t last = 0;
    EXPECT_EQ(wal.Replay(table, last).code(), StatusCode::kIOError);
    EXPECT_EQ(last, 0U);
  }
}

TEST(WalTest, AsyncAppendDoesNotSyncAndSyncAppendDoes) {
  auto file = std::make_unique<MemoryFile>();
  auto* memory = file.get();
  Wal wal(std::move(file));
  WriteBatch batch;
  ASSERT_TRUE(batch.Put("", "").ok());
  ASSERT_TRUE(wal.Append(batch, 1, false).ok());
  EXPECT_EQ(memory->syncs, 0U);
  ASSERT_TRUE(wal.Append(batch, 2, true).ok());
  EXPECT_EQ(memory->syncs, 1U);
  EXPECT_EQ(wal.SyncCount(), 1U);
  EXPECT_EQ(wal.BytesWritten(), memory->bytes.size());
}
}  // namespace
}  // namespace lsm::internal
