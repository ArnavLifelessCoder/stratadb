#include "platform/mapping.hpp"

#include <gtest/gtest.h>

#include "platform/file.hpp"
#include "temp_directory.hpp"

namespace lsm::platform {
namespace {
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
