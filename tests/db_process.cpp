#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "lsm/db.hpp"

namespace {
int Fail(const lsm::Status& status) {
  std::cerr << status.ToString() << '\n';
  return 1;
}

int Run(const std::filesystem::path& path, bool sync, bool writer, std::uint64_t count) {
  std::unique_ptr<lsm::DB> db;
  auto status = lsm::DB::Open({.memtable_bytes = 1}, path, &db);
  if (!status.ok()) {
    return Fail(status);
  }
  bool missing_tail = false;
  for (std::uint64_t i = 0; writer || i < count; ++i) {
    const auto key = std::to_string(i);
    const auto expected = "value-" + key;
    if (writer) {
      lsm::WriteBatch batch;
      status = batch.Put(key, expected);
      if (status.ok()) {
        status = batch.Put("latest", key);
      }
      if (status.ok()) {
        status = db->Write({sync}, &batch);
      }
      if (!status.ok()) {
        return Fail(status);
      }
      // The parent records acknowledgments only after Write has returned.
      std::cout << i + 1 << std::endl;
    } else {
      std::string value;
      status = db->Get({}, key, &value);
      if (!sync && status.IsNotFound()) {
        missing_tail = true;
        continue;
      }
      if (!status.ok()) {
        return Fail(status);
      }
      if (value != expected || missing_tail) {
        return Fail(lsm::Status::Corruption("recovered value differs from acknowledgment"));
      }
    }
  }
  std::string latest;
  status = db->Get({}, "latest", &latest);
  if (!status.ok()) {
    return !sync && status.IsNotFound() && missing_tail ? 0 : Fail(status);
  }
  std::string latest_value;
  status = db->Get({}, latest, &latest_value);
  if (!status.ok() || latest_value != "value-" + latest) {
    return Fail(lsm::Status::Corruption("partially recovered batch"));
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    return 2;
  }
  std::uint64_t count = 0;
  const std::string_view number(argv[4]);
  const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), count);
  if (error != std::errc{} || end != number.data() + number.size()) {
    return 2;
  }
  return Run(std::filesystem::path(argv[2]), std::string_view(argv[3]) == "sync",
             std::string_view(argv[1]) == "write", count);
}
