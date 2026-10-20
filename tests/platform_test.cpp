#include <gtest/gtest.h>

#include "platform/file.hpp"
#include "temp_directory.hpp"

namespace lsm::platform {
namespace {
TEST(PlatformTest, SyncRenameReplaceTruncateAppendAndUnicodePaths) {
  test::TempDirectory directory;
  const auto parent = directory.path() / std::filesystem::path(u8"数据") / "nested";
  ASSERT_TRUE(CreateDirectories(parent).ok());
  const auto source = parent / "temp";
  const auto destination = parent / "WAL";
  std::unique_ptr<File> file;
  ASSERT_TRUE(File::Open(destination, true, file).ok());
  ASSERT_TRUE(file->Append("old").ok());
  ASSERT_TRUE(file->Sync().ok());
  file.reset();
  ASSERT_TRUE(File::Open(source, true, file).ok());
  const std::string bytes("new\0data", 8);
  ASSERT_TRUE(file->Append(bytes).ok());
  ASSERT_TRUE(file->Sync().ok());
  file.reset();
  ASSERT_TRUE(AtomicRename(source, destination).ok());
  ASSERT_TRUE(File::Open(destination, false, file).ok());
  std::string output;
  ASSERT_TRUE(file->ReadAll(output).ok());
  EXPECT_EQ(output, bytes);
  ASSERT_TRUE(file->Truncate(3).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Append("er").ok());
  ASSERT_TRUE(file->Sync().ok());
  file.reset();
  ASSERT_TRUE(File::Open(destination, false, file).ok());
  ASSERT_TRUE(file->ReadAll(output).ok());
  EXPECT_EQ(output, "newer");
}

TEST(PlatformTest, ExclusiveLockAndErrors) {
  test::TempDirectory directory;
  const auto path = directory.path() / "LOCK";
  std::unique_ptr<File> first;
  std::unique_ptr<File> second;
  EXPECT_FALSE(File::Open(path, false, first).ok());
  ASSERT_TRUE(File::Open(path, true, first).ok());
  ASSERT_TRUE(first->Lock().ok());
  ASSERT_TRUE(File::Open(path, false, second).ok());
  EXPECT_FALSE(second->Lock().ok());
  first.reset();
  EXPECT_TRUE(second->Lock().ok());
  EXPECT_FALSE(AtomicRename(directory.path() / "missing", directory.path() / "target").ok());
  EXPECT_FALSE(CreateDirectories(path / "child").ok());
}
}  // namespace
}  // namespace lsm::platform
