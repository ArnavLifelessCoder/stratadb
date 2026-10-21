// Child process for python/crash_test.py.
//
//   lsm_crash_writer write <db> <sync|async> <seed> <first_batch> <first_op> <log>
//   lsm_crash_writer dump <db> <output>
//
// The writer appends a batch's intent to <log> before calling the DB and an
// acknowledgement line only after the call has returned OK. The parent kills
// the process at a random time and then runs dump, which reopens the DB and
// prints the op id behind every live key.

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/db.hpp"

namespace {
// Enough distinct keys and value bytes for L1 to pass its 10 MB target, so kills
// also land in L1 -> L2 compactions, not only flushes and L0 -> L1.
constexpr unsigned kKeys = 20000;

// Small memtables and an eager L0 trigger keep flushes and compactions running
// while the parent picks its kill point.
auto CrashOptions() -> lsm::Options {
  return {.memtable_bytes = 64 << 10, .block_bytes = 1024, .l0_compaction_trigger = 2};
}

auto Fail(std::string_view what, const lsm::Status& status) -> int {
  std::cerr << what << ": " << status.ToString() << '\n';
  return 1;
}

auto Key(unsigned index) -> std::string {
  auto key = std::to_string(index);
  return "k" + std::string(5 - key.size(), '0') + key;
}

// "<op>:" followed by filler derived from the op id, so dump can detect damage.
auto Value(std::uint64_t op, std::size_t filler) -> std::string {
  auto value = std::to_string(op) + ":";
  for (std::size_t i = 0; i < filler; ++i) {
    value.push_back(static_cast<char>('a' + (op * 7 + i) % 26));
  }
  return value;
}

auto ParseValue(std::string_view value, std::uint64_t& op) -> bool {
  const auto colon = value.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  const auto [end, error] = std::from_chars(value.data(), value.data() + colon, op);
  if (error != std::errc{} || end != value.data() + colon) {
    return false;
  }
  const auto filler = value.substr(colon + 1);
  for (std::size_t i = 0; i < filler.size(); ++i) {
    if (filler[i] != static_cast<char>('a' + (op * 7 + i) % 26)) {
      return false;
    }
  }
  return true;
}

auto ParseNumber(std::string_view text, std::uint64_t& output) -> bool {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
  return error == std::errc{} && end == text.data() + text.size();
}

// flush() hands each line to the OS, which keeps it across a process kill.
class Log {
 public:
  explicit Log(const std::filesystem::path& path) : file_(path, std::ios::binary | std::ios::app) {}
  [[nodiscard]] auto ok() const -> bool { return file_.good(); }
  auto Line(const std::string& line) -> bool {
    file_ << line << '\n';
    file_.flush();
    return file_.good();
  }

 private:
  std::ofstream file_;
};

auto Write(const std::filesystem::path& path, bool sync, std::uint64_t seed, std::uint64_t batch_id,
           std::uint64_t op_id, const std::filesystem::path& log_path) -> int {
  Log log(log_path);
  if (!log.ok()) {
    std::cerr << "cannot open log\n";
    return 1;
  }
  std::unique_ptr<lsm::DB> db;
  const auto status = lsm::DB::Open(CrashOptions(), path, &db);
  if (!status.ok()) {
    return Fail("open", status);
  }
  std::cout << "ready" << std::endl;
  std::mt19937_64 random(seed);
  for (;; ++batch_id) {
    const auto shape = random() % 100;
    const std::size_t count = shape < 85 ? 1 : 2 + random() % 7;
    lsm::WriteBatch batch;
    std::string intent = "B " + std::to_string(batch_id) + " " + std::to_string(count);
    for (std::size_t i = 0; i < count; ++i, ++op_id) {
      const auto key = Key(static_cast<unsigned>(random() % kKeys));
      if (random() % 100 < 20) {
        intent += "\nD " + std::to_string(op_id) + " " + key;
        if (!batch.Delete(key).ok()) {
          return 1;
        }
      } else {
        const std::size_t filler = random() % 100 == 0 ? 20000 : random() % 2000;
        intent += "\nP " + std::to_string(op_id) + " " + key;
        if (!batch.Put(key, Value(op_id, filler)).ok()) {
          return 1;
        }
      }
    }
    if (!log.Line(intent)) {
      std::cerr << "cannot write intent\n";
      return 1;
    }
    const lsm::WriteOptions options{.sync = sync};
    lsm::Status result;
    if (count == 1) {
      // Exercise the single-operation entry points too.
      const auto& entry = batch.Entries().front();
      result = entry.type == lsm::WriteBatch::Type::kPut ? db->Put(options, entry.key, entry.value)
                                                         : db->Delete(options, entry.key);
    } else {
      result = db->Write(options, &batch);
    }
    if (!result.ok()) {
      return Fail("write", result);
    }
    if (!log.Line("A " + std::to_string(batch_id))) {
      std::cerr << "cannot write acknowledgement\n";
      return 1;
    }
  }
}

auto Dump(const std::filesystem::path& path, const std::filesystem::path& output) -> int {
  std::unique_ptr<lsm::DB> db;
  auto status = lsm::DB::Open(CrashOptions(), path, &db);
  if (!status.ok()) {
    return Fail("recovery open", status);
  }
  std::map<std::string, std::uint64_t, std::less<>> live;
  {
    auto iterator = db->NewIterator({});
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
      std::uint64_t op = 0;
      if (!ParseValue(iterator->value(), op)) {
        std::cerr << "damaged value for key " << iterator->key() << '\n';
        return 1;
      }
      live.emplace(iterator->key(), op);
    }
    if (!iterator->status().ok()) {
      return Fail("scan", iterator->status());
    }
  }
  // Point reads must agree with the scan for every key the writer can touch.
  for (unsigned index = 0; index < kKeys; ++index) {
    const auto key = Key(index);
    std::string value;
    status = db->Get({}, key, &value);
    const auto found = live.find(key);
    if (status.IsNotFound() && found == live.end()) {
      continue;
    }
    std::uint64_t op = 0;
    if (!status.ok() || found == live.end() || !ParseValue(value, op) || op != found->second) {
      std::cerr << "Get and scan disagree for key " << key << '\n';
      return 1;
    }
  }
  std::ofstream file(output, std::ios::binary | std::ios::trunc);
  // First line: table counts per level, as evidence that flushes and
  // compactions happened between kills.
  file << "#levels";
  for (const auto count : db->GetStats().level_file_counts) {
    file << ' ' << count;
  }
  file << '\n';
  for (const auto& [key, op] : live) {
    file << key << ' ' << op << '\n';
  }
  file.close();
  if (!file) {
    std::cerr << "cannot write dump output\n";
    return 1;
  }
  return 0;
}
}  // namespace

auto main(int argc, char** argv) -> int {
  const std::vector<std::string_view> args(argv, argv + argc);
  if (args.size() == 8 && args[1] == "write") {
    std::uint64_t seed = 0;
    std::uint64_t batch = 0;
    std::uint64_t op = 0;
    if (!ParseNumber(args[4], seed) || !ParseNumber(args[5], batch) || !ParseNumber(args[6], op) ||
        (args[3] != "sync" && args[3] != "async")) {
      return 2;
    }
    return Write(std::filesystem::path(args[2]), args[3] == "sync", seed, batch, op,
                 std::filesystem::path(args[7]));
  }
  if (args.size() == 4 && args[1] == "dump") {
    return Dump(std::filesystem::path(args[2]), std::filesystem::path(args[3]));
  }
  std::cerr << "usage: lsm_crash_writer write <db> <sync|async> <seed> <first_batch> <first_op> "
               "<log> | dump <db> <output>\n";
  return 2;
}
