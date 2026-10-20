#include "version_set.hpp"

#include <gtest/gtest.h>

#include <limits>

#include "table/format.hpp"
#include "temp_directory.hpp"

namespace lsm::internal {
namespace {
class ManifestFile final : public platform::File {
 public:
  std::string bytes;
  bool fail_read = false;
  bool fail_sync = false;
  bool fail_truncate = false;
  bool fail_append = false;
  unsigned syncs = 0;
  unsigned appends = 0;
  auto ReadAll(std::string& out) -> Status override {
    if (fail_read) {
      return Status::IOError("injected read");
    }
    out = bytes;
    return {};
  }
  auto Append(std::string_view input) -> Status override {
    ++appends;
    bytes.append(input.substr(0, fail_append ? input.size() / 2 : input.size()));
    return fail_append ? Status::IOError("injected append") : Status{};
  }
  auto Sync() -> Status override {
    ++syncs;
    return fail_sync ? Status::IOError("injected sync") : Status{};
  }
  auto Truncate(std::uint64_t size) -> Status override {
    if (fail_truncate) {
      return Status::IOError("injected truncate");
    }
    bytes.resize(static_cast<std::size_t>(size));
    return {};
  }
  auto Lock() -> Status override { return {}; }
};

auto Record(const VersionEdit& edit) -> std::string {
  std::string payload;
  EXPECT_TRUE(EncodeEdit(edit, payload).ok());
  std::string body;
  table::AppendFixed<4>(body, payload.size());
  body.push_back('\1');
  body.append(payload);
  std::string result;
  table::AppendFixed<4>(result, Crc32c(body));
  result.append(body);
  return result;
}

auto Rotation() -> VersionEdit {
  return {.next_file = 5,
          .last_sequence = 2,
          .flushed_sequence = 0,
          .current_wal = 3,
          .previous_wal = 2,
          .pending_table = 4,
          .added = {},
          .removed = {}};
}

TEST(ManifestTest, CodecRoundTripTruncationAndInvalidVarintsPreserveOutput) {
  auto edit = Rotation();
  edit.added.push_back({.number = 10,
                        .level = 2,
                        .size = 1000,
                        .smallest = std::string("\0a", 2),
                        .largest = "\xff",
                        .reader = {}});
  edit.removed = {8, 9};
  std::string bytes;
  ASSERT_TRUE(EncodeEdit(edit, bytes).ok());
  VersionEdit decoded;
  ASSERT_TRUE(DecodeEdit(bytes, decoded).ok());
  std::string encoded;
  ASSERT_TRUE(EncodeEdit(decoded, encoded).ok());
  EXPECT_EQ(encoded, bytes);
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    EXPECT_EQ(DecodeEdit(std::string_view(bytes).substr(0, size), decoded).code(),
              StatusCode::kCorruption);
    EXPECT_EQ(decoded.next_file, edit.next_file);
  }
  EXPECT_EQ(DecodeEdit(bytes + "extra", decoded).code(), StatusCode::kCorruption);
  bytes[0] = '\0';
  EXPECT_EQ(DecodeEdit(bytes, decoded).code(), StatusCode::kCorruption);
}

TEST(ManifestTest, EveryTornTailAndZeroTailIsRepairedAndMiddleDamageIsRejected) {
  const auto first = Record({});
  const auto second = Record(Rotation());
  for (std::size_t size = 0; size < second.size(); ++size) {
    auto file = std::make_unique<ManifestFile>();
    auto* memory = file.get();
    memory->bytes = first + second.substr(0, size);
    std::unique_ptr<VersionSet> versions;
    ASSERT_TRUE(VersionSet::Recover({}, std::move(file), versions).ok());
    EXPECT_EQ(memory->bytes, first);
    EXPECT_EQ(memory->syncs, size == 0 ? 0U : 1U);
    EXPECT_EQ(versions->State().metadata.next_file, 3U);
  }
  for (std::size_t byte = 0; byte < first.size(); ++byte) {
    auto file = std::make_unique<ManifestFile>();
    file->bytes = first + second;
    file->bytes[byte] ^= 0x40;
    std::unique_ptr<VersionSet> versions;
    EXPECT_EQ(VersionSet::Recover({}, std::move(file), versions).code(), StatusCode::kCorruption);
  }
  for (const bool zero : {false, true}) {
    auto file = std::make_unique<ManifestFile>();
    auto* memory = file.get();
    memory->bytes = first + (zero ? std::string(512, '\0') : second);
    if (!zero) {
      memory->bytes.back() ^= 1;
    }
    std::unique_ptr<VersionSet> versions;
    ASSERT_TRUE(VersionSet::Recover({}, std::move(file), versions).ok());
    EXPECT_EQ(memory->bytes, first);
    EXPECT_EQ(memory->syncs, 1U);
  }
}

TEST(ManifestTest, IoFailuresAreStickyAndNeverPublishANewVersion) {
  for (const bool sync : {false, true}) {
    auto file = std::make_unique<ManifestFile>();
    auto* memory = file.get();
    memory->bytes = Record({});
    std::unique_ptr<VersionSet> versions;
    ASSERT_TRUE(VersionSet::Recover({}, std::move(file), versions).ok());
    memory->fail_sync = sync;
    memory->fail_append = !sync;
    const auto held = versions->State().version;
    EXPECT_EQ(versions->Apply(Rotation()).code(), StatusCode::kIOError);
    EXPECT_EQ(versions->Apply(Rotation()).code(), StatusCode::kIOError);
    EXPECT_EQ(memory->appends, 1U);
    EXPECT_EQ(versions->State().version, held);
    EXPECT_EQ(versions->State().metadata.current_wal, 2U);
  }
  for (int mode = 0; mode < 3; ++mode) {
    auto file = std::make_unique<ManifestFile>();
    file->bytes = Record({}) + "tail";
    file->fail_read = mode == 0;
    file->fail_truncate = mode == 1;
    file->fail_sync = mode == 2;
    std::unique_ptr<VersionSet> versions;
    EXPECT_EQ(VersionSet::Recover({}, std::move(file), versions).code(), StatusCode::kIOError);
  }
}

TEST(ManifestTest, RejectsRegressingCountersAndInvalidFileState) {
  auto file = std::make_unique<ManifestFile>();
  file->bytes = Record({});
  std::unique_ptr<VersionSet> versions;
  ASSERT_TRUE(VersionSet::Recover({}, std::move(file), versions).ok());
  for (int mode = 0; mode < 6; ++mode) {
    auto edit = Rotation();
    if (mode == 0) edit.next_file = 2;
    if (mode == 1) edit.last_sequence = kMaxSequence + 1;
    if (mode == 2) edit.flushed_sequence = 3;
    if (mode == 3) edit.pending_table = 0;
    if (mode == 4) edit.current_wal = edit.previous_wal;
    if (mode == 5) edit.removed = {99};
    EXPECT_EQ(versions->Apply(edit).code(), StatusCode::kInvalidArgument);
  }
}

TEST(ManifestTest, NativeCurrentPublicationRecoveryAndVersionOwnership) {
  test::TempDirectory directory;
  std::unique_ptr<VersionSet> versions;
  ASSERT_TRUE(VersionSet::Open(directory.path(), versions).ok());
  const std::vector<table::Entry> entries{
      {.key = table::InternalKey("key", 1, WriteBatch::Type::kPut), .value = "value"}};
  const auto path = TablePath(directory.path(), 3);
  ASSERT_TRUE(table::WriteTable(path, entries).ok());
  auto edit = versions->State().metadata;
  edit.next_file = 4;
  edit.last_sequence = 1;
  edit.flushed_sequence = 1;
  edit.added.push_back({.number = 3,
                        .level = 0,
                        .size = std::filesystem::file_size(path),
                        .smallest = "key",
                        .largest = "key",
                        .reader = {}});
  ASSERT_TRUE(versions->Apply(edit).ok());
  auto held = versions->State().version;
  versions.reset();
  ASSERT_TRUE(VersionSet::Open(directory.path(), versions).ok());
  ASSERT_EQ(versions->State().version->files.size(), 1U);
  edit = versions->State().metadata;
  edit.removed = {3};
  ASSERT_TRUE(versions->Apply(edit).ok());
  EXPECT_TRUE(versions->State().version->files.empty());
  table::Entry value;
  ASSERT_TRUE(held->files.front().reader->Get("key", 1, value).ok());
  EXPECT_EQ(value.value, "value");
  // Simulate a crash after manifest publication but before CURRENT publication.
  versions.reset();
  ASSERT_TRUE(std::filesystem::remove(directory.path() / "CURRENT"));
  ASSERT_TRUE(VersionSet::Open(directory.path(), versions).ok());
  EXPECT_TRUE(std::filesystem::exists(directory.path() / "CURRENT"));
  versions.reset();
  ASSERT_TRUE(WriteAtomicFile(directory.path() / "CURRENT", "../outside\n").ok());
  EXPECT_EQ(VersionSet::Open(directory.path(), versions).code(), StatusCode::kCorruption);
}

TEST(ManifestTest, BootstrapCountersIncludeInitialManifestAndCurrentWrites) {
  test::TempDirectory directory;
  std::unique_ptr<VersionSet> versions;
  ASSERT_TRUE(VersionSet::Open(directory.path(), versions).ok());
  const auto expected = std::filesystem::file_size(directory.path() / "CURRENT") +
                        std::filesystem::file_size(directory.path() / "MANIFEST-1") +
                        std::filesystem::file_size(WalPath(directory.path(), 2));
  EXPECT_EQ(versions->BytesWritten(), expected);
}

TEST(ManifestTest, IncompleteBootstrapPreservesTheAlreadyPublishedWal) {
  test::TempDirectory directory;
  ASSERT_TRUE(WriteAtomicFile(WalPath(directory.path(), 2), "existing WAL bytes").ok());
  std::unique_ptr<VersionSet> versions;
  ASSERT_TRUE(VersionSet::Open(directory.path(), versions).ok());
  std::unique_ptr<platform::File> file;
  ASSERT_TRUE(platform::File::Open(WalPath(directory.path(), 2), false, file).ok());
  std::string bytes;
  ASSERT_TRUE(file->ReadAll(bytes).ok());
  EXPECT_EQ(bytes, "existing WAL bytes");
}

TEST(ManifestTest, MissingMetadataWithLaterFilesNeverInitializesAnEmptyDatabase) {
  for (const bool table_file : {false, true}) {
    test::TempDirectory directory;
    const auto path = table_file ? TablePath(directory.path(), 4) : WalPath(directory.path(), 3);
    ASSERT_TRUE(WriteAtomicFile(path, "existing data").ok());
    std::unique_ptr<VersionSet> versions;
    EXPECT_EQ(VersionSet::Open(directory.path(), versions).code(), StatusCode::kCorruption);
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "CURRENT"));
  }
}
}  // namespace
}  // namespace lsm::internal
