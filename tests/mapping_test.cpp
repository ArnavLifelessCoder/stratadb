#include "platform/mapping.hpp"

#include <gtest/gtest.h>

#include "platform/file.hpp"
#include "temp_directory.hpp"

namespace lsm::platform {
namespace {
TEST(MappingTest, DeletionAttemptWhileMappedPreservesBytesAndEventuallyReclaimsFile) {
  test::TempDirectory directory;
  const auto path = directory.path() / "obsolete.sst";
  std::unique_ptr<File> file;
  ASSERT_TRUE(File::Open(path, true, file).ok());
  ASSERT_TRUE(file->Append("mapped bytes").ok());
  ASSERT_TRUE(file->Sync().ok());
  file.reset();
  std::unique_ptr<MappedFile> mapping;
  ASSERT_TRUE(MappedFile::Open(path, mapping).ok());
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  // Windows may defer or reject unlink while a section is mapped. POSIX
  // unlinks immediately. Both retain the mapping until its owner releases it.
  EXPECT_TRUE(removed || error);
  EXPECT_EQ(mapping->Bytes(), "mapped bytes");
  mapping.reset();
  error.clear();
  std::filesystem::remove(path, error);
  EXPECT_FALSE(error) << error.message();
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(MappingTest, EmptyBinaryLargeUnicodeAndMissingFiles) {
  test::TempDirectory directory;
  const auto path = directory.path() / std::filesystem::path(u8"mapped-\u03bb");
  for (const std::size_t size : {0U, 1U, 4097U, 1048576U}) {
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
      bytes[i] = static_cast<char>(i % 256);
    }
    std::unique_ptr<File> file;
    ASSERT_TRUE(File::Open(path, true, file).ok());
    ASSERT_TRUE(file->Truncate(0).ok());
    ASSERT_TRUE(file->Append(bytes).ok());
    ASSERT_TRUE(file->Sync().ok());
    file.reset();
    std::unique_ptr<MappedFile> mapping;
    ASSERT_TRUE(MappedFile::Open(path, mapping).ok());
    EXPECT_EQ(mapping->Bytes(), bytes);
    const auto* previous = mapping.get();
    EXPECT_EQ(MappedFile::Open(directory.path() / "absent", mapping).code(), StatusCode::kIOError);
    EXPECT_EQ(mapping.get(), previous);
    EXPECT_EQ(MappedFile::Open(directory.path(), mapping).code(), StatusCode::kIOError);
  }
}
}  // namespace
}  // namespace lsm::platform
