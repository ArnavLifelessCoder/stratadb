#include "lsm/status.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace lsm {
namespace {

TEST(StatusTest, DefaultIsSuccess) {
  const Status status;
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kOk);
  EXPECT_TRUE(status.message().empty());
  EXPECT_FALSE(status.IsNotFound());
  EXPECT_EQ(status.ToString(), "OK");
}

TEST(StatusTest, ErrorsKeepCodeAndMessage) {
  const std::array errors{Status::NotFound("key"), Status::InvalidArgument("option"),
                          Status::Corruption("crc"), Status::IOError("sync")};
  const std::array codes{StatusCode::kNotFound, StatusCode::kInvalidArgument,
                         StatusCode::kCorruption, StatusCode::kIOError};
  const std::array messages{"key", "option", "crc", "sync"};
  const std::array descriptions{"NotFound: key", "InvalidArgument: option", "Corruption: crc",
                                "IOError: sync"};
  for (std::size_t i = 0; i < errors.size(); ++i) {
    EXPECT_FALSE(errors[i].ok());
    EXPECT_EQ(errors[i].code(), codes[i]);
    EXPECT_EQ(errors[i].message(), messages[i]);
    EXPECT_EQ(errors[i].ToString(), descriptions[i]);
    EXPECT_EQ(errors[i].IsNotFound(), i == 0);
  }
}

TEST(StatusTest, MessageOwnsBinaryDataAndCopiesIndependently) {
  std::string message("a\0b", 3);
  const auto status = Status::Corruption(message);
  const auto copy = status;
  message.assign("changed");
  EXPECT_EQ(copy.message(), std::string("a\0b", 3));
  EXPECT_EQ(copy.ToString(), std::string("Corruption: a\0b", 15));
  EXPECT_EQ(Status::IOError("").ToString(), "IOError");
}

}  // namespace
}  // namespace lsm
