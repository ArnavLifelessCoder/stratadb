// stratadb explorer: a local HTTP server around one DB and a web page that
// shows it working. Point reads and writes, ordered scans, snapshots, a bulk
// loader, and the engine's live statistics (memtable, levels, flushes,
// compactions, stalls, cache).
//
//   lsm_server [--db=PATH] [--port=8080] [--web=DIR] [--memtable_bytes=N]
//
// The server listens on 127.0.0.1 only. Keys and values are bytes; JSON output
// escapes control characters and every byte >= 0x80 as \u00XX.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "httplib.h"
#include "lsm/db.hpp"

#ifndef LSM_EXPLORER_WEB_DIR
#define LSM_EXPLORER_WEB_DIR "explorer/web"
#endif

namespace {

auto Json(std::string_view text) -> std::string {
  std::string out = "\"";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (byte < 0x20 || byte >= 0x80) {
      static constexpr char kHex[] = "0123456789abcdef";
      out += "\\u00";
      out += kHex[byte >> 4];
      out += kHex[byte & 0xf];
    } else {
      out += c;
    }
  }
  return out + "\"";
}

auto Number(const httplib::Request& request, const std::string& name, std::uint64_t fallback,
            std::uint64_t maximum, std::uint64_t& out) -> bool {
  if (!request.has_param(name)) {
    out = fallback;
    return true;
  }
  try {
    std::size_t used = 0;
    const auto& text = request.get_param_value(name);
    out = std::stoull(text, &used);
    return used == text.size() && out <= maximum;
  } catch (const std::exception&) {
    return false;
  }
}

void Reply(httplib::Response& response, int status, const std::string& body) {
  response.status = status;
  response.set_content(body, "application/json");
}

void Fail(httplib::Response& response, int status, std::string_view message) {
  Reply(response, status, "{\"error\":" + Json(message) + "}");
}

// Bulk loader state, shown on the page while it runs.
struct Load {
  std::atomic<bool> running{false};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> done{0};
  std::atomic<std::uint64_t> total{0};
  std::atomic<std::uint64_t> started_ms{0};
  std::atomic<std::uint64_t> finished_ms{0};
  std::mutex mutex;  // Guards `error` and the thread handle.
  std::string error;
  std::thread thread;
};

auto NowMs() -> std::uint64_t {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

class Explorer {
 public:
  Explorer(std::unique_ptr<lsm::DB> db, lsm::Options options)
      : db_(std::move(db)), options_(options) {}
  Explorer(const Explorer&) = delete;
  auto operator=(const Explorer&) -> Explorer& = delete;
  ~Explorer() {
    StopLoad();
    const std::lock_guard guard(snapshots_mutex_);
    for (const auto& [id, snapshot] : snapshots_) db_->ReleaseSnapshot(snapshot);
  }

  void Register(httplib::Server& server) {
    server.Get("/api/stats", [this](const auto&, auto& res) { Stats(res); });
    server.Get("/api/get", [this](const auto& req, auto& res) { Get(req, res); });
    server.Post("/api/put", [this](const auto& req, auto& res) { Put(req, res); });
    server.Post("/api/delete", [this](const auto& req, auto& res) { Delete(req, res); });
    server.Get("/api/scan", [this](const auto& req, auto& res) { Scan(req, res); });
    server.Post("/api/snapshot", [this](const auto&, auto& res) { TakeSnapshot(res); });
    server.Post("/api/snapshot/release",
                [this](const auto& req, auto& res) { ReleaseSnapshot(req, res); });
    server.Post("/api/load", [this](const auto& req, auto& res) { StartLoad(req, res); });
    server.Post("/api/load/stop", [this](const auto&, auto& res) {
      StopLoad();
      Reply(res, 200, "{\"ok\":true}");
    });
  }

 private:
  // Resolves the optional `snapshot` parameter. False means a bad id.
  auto ReadOptions(const httplib::Request& request, lsm::ReadOptions& read) -> bool {
    if (!request.has_param("snapshot") || request.get_param_value("snapshot").empty()) return true;
    std::uint64_t id = 0;
    if (!Number(request, "snapshot", 0, UINT64_MAX, id)) return false;
    const std::lock_guard guard(snapshots_mutex_);
    const auto found = snapshots_.find(id);
    if (found == snapshots_.end()) return false;
    read.snapshot = found->second;
    return true;
  }

  void Stats(httplib::Response& response) {
    const auto stats = db_->GetStats();
    std::ostringstream json;
    json << "{\"user_bytes_written\":" << stats.user_bytes_written
         << ",\"disk_bytes_written\":" << stats.disk_bytes_written
         << ",\"wal_syncs\":" << stats.wal_syncs << ",\"write_calls\":" << stats.write_calls
         << ",\"wal_records\":" << stats.wal_records << ",\"flushes\":" << stats.flushes
         << ",\"compactions\":" << stats.compactions << ",\"stall_micros\":" << stats.stall_micros
         << ",\"slowed_writes\":" << stats.slowed_writes
         << ",\"stopped_writes\":" << stats.stopped_writes
         << ",\"block_cache_hits\":" << stats.block_cache_hits
         << ",\"block_cache_misses\":" << stats.block_cache_misses
         << ",\"block_cache_bytes\":" << stats.block_cache_bytes
         << ",\"block_cache_capacity\":" << options_.block_cache_bytes
         << ",\"memtable_bytes\":" << stats.memtable_bytes
         << ",\"memtable_limit\":" << options_.memtable_bytes
         << ",\"l0_compaction_trigger\":" << options_.l0_compaction_trigger
         << ",\"l0_stop_writes\":" << options_.l0_stop_writes
         << ",\"level_size_ratio\":" << options_.level_size_ratio << ",\"levels\":[";
    for (std::size_t level = 0; level < stats.level_file_counts.size(); ++level) {
      json << (level ? "," : "") << "{\"files\":" << stats.level_file_counts[level]
           << ",\"bytes\":" << stats.level_bytes[level] << "}";
    }
    json << "],\"snapshots\":[";
    {
      const std::lock_guard guard(snapshots_mutex_);
      bool first = true;
      for (const auto& [id, snapshot] : snapshots_) {
        json << (first ? "" : ",") << id;
        first = false;
      }
    }
    const auto now = NowMs();
    const auto finished = load_.finished_ms.load();
    const auto elapsed = (load_.running ? now : finished) - load_.started_ms.load();
    std::string load_error;
    {
      const std::lock_guard guard(load_.mutex);
      load_error = load_.error;
    }
    json << "],\"load\":{\"running\":" << (load_.running ? "true" : "false")
         << ",\"done\":" << load_.done << ",\"total\":" << load_.total
         << ",\"elapsed_ms\":" << (load_.started_ms ? elapsed : 0)
         << ",\"error\":" << Json(load_error) << "}}";
    Reply(response, 200, json.str());
  }

  void Get(const httplib::Request& request, httplib::Response& response) {
    if (!request.has_param("key")) return Fail(response, 400, "missing key");
    lsm::ReadOptions read;
    if (!ReadOptions(request, read)) return Fail(response, 400, "unknown snapshot");
    std::string value;
    const auto status = db_->Get(read, request.get_param_value("key"), &value);
    if (status.IsNotFound()) return Reply(response, 200, "{\"found\":false}");
    if (!status.ok()) return Fail(response, 500, status.ToString());
    Reply(response, 200, "{\"found\":true,\"value\":" + Json(value) + "}");
  }

  void Put(const httplib::Request& request, httplib::Response& response) {
    if (!request.has_param("key")) return Fail(response, 400, "missing key");
    const lsm::WriteOptions write{.sync = request.get_param_value("sync") == "true"};
    const auto status =
        db_->Put(write, request.get_param_value("key"), request.get_param_value("value"));
    if (!status.ok()) return Fail(response, 500, status.ToString());
    Reply(response, 200, "{\"ok\":true}");
  }

  void Delete(const httplib::Request& request, httplib::Response& response) {
    if (!request.has_param("key")) return Fail(response, 400, "missing key");
    const lsm::WriteOptions write{.sync = request.get_param_value("sync") == "true"};
    const auto status = db_->Delete(write, request.get_param_value("key"));
    if (!status.ok()) return Fail(response, 500, status.ToString());
    Reply(response, 200, "{\"ok\":true}");
  }

  void Scan(const httplib::Request& request, httplib::Response& response) {
    std::uint64_t limit = 0;
    if (!Number(request, "limit", 50, 1000, limit)) return Fail(response, 400, "bad limit");
    lsm::ReadOptions read;
    if (!ReadOptions(request, read)) return Fail(response, 400, "unknown snapshot");
    auto iterator = db_->NewIterator(read);
    const auto start = request.get_param_value("start");
    if (start.empty()) {
      iterator->SeekToFirst();
    } else {
      iterator->Seek(start);
    }
    std::string json = "{\"entries\":[";
    std::uint64_t count = 0;
    for (; iterator->Valid() && count < limit; iterator->Next(), ++count) {
      json += (count ? "," : "");
      json += "{\"key\":" + Json(iterator->key()) + ",\"value\":" + Json(iterator->value()) + "}";
    }
    if (!iterator->status().ok()) return Fail(response, 500, iterator->status().ToString());
    json += "],\"more\":";
    json += iterator->Valid() ? "true" : "false";
    Reply(response, 200, json + "}");
  }

  void TakeSnapshot(httplib::Response& response) {
    const std::lock_guard guard(snapshots_mutex_);
    const auto id = ++next_snapshot_;
    snapshots_.emplace(id, db_->GetSnapshot());
    Reply(response, 200, "{\"id\":" + std::to_string(id) + "}");
  }

  void ReleaseSnapshot(const httplib::Request& request, httplib::Response& response) {
    std::uint64_t id = 0;
    if (!Number(request, "id", 0, UINT64_MAX, id)) return Fail(response, 400, "bad id");
    const std::lock_guard guard(snapshots_mutex_);
    const auto found = snapshots_.find(id);
    if (found == snapshots_.end()) return Fail(response, 404, "unknown snapshot");
    db_->ReleaseSnapshot(found->second);
    snapshots_.erase(found);
    Reply(response, 200, "{\"ok\":true}");
  }

  // Writes `count` keys "<prefix><index>" with `value_size`-byte values in
  // batches of 100, in index order or shuffled, on a background thread.
  void StartLoad(const httplib::Request& request, httplib::Response& response) {
    std::uint64_t count = 0;
    std::uint64_t value_size = 0;
    if (!Number(request, "count", 100000, 20000000, count) || count == 0) {
      return Fail(response, 400, "count must be 1..20000000");
    }
    if (!Number(request, "value_size", 100, 65536, value_size)) {
      return Fail(response, 400, "value_size must be 0..65536");
    }
    const bool random_order = request.get_param_value("order") != "sequential";
    const auto prefix = request.has_param("prefix") ? request.get_param_value("prefix") : "key";
    const std::lock_guard guard(load_.mutex);
    if (load_.running) return Fail(response, 409, "a load is already running");
    if (load_.thread.joinable()) load_.thread.join();
    load_.error.clear();
    load_.stop = false;
    load_.done = 0;
    load_.total = count;
    load_.started_ms = NowMs();
    load_.running = true;
    load_.thread = std::thread([this, count, value_size, random_order, prefix] {
      std::mt19937_64 random(NowMs());
      const auto width = std::to_string(count - 1).size();
      std::string value(value_size, 'v');
      for (std::uint64_t base = 0; base < count && !load_.stop; base += 100) {
        lsm::WriteBatch batch;
        for (std::uint64_t i = base; i < std::min(count, base + 100); ++i) {
          const auto index = random_order ? random() % count : i;
          auto digits = std::to_string(index);
          digits.insert(0, width - digits.size(), '0');
          for (auto& c : value) c = static_cast<char>('a' + random() % 26);
          static_cast<void>(batch.Put(prefix + digits, value));
        }
        const auto status = db_->Write({}, &batch);
        if (!status.ok()) {
          const std::lock_guard error_guard(load_.mutex);
          load_.error = status.ToString();
          break;
        }
        load_.done += batch.Count();
      }
      load_.finished_ms = NowMs();
      load_.running = false;
    });
    Reply(response, 200, "{\"ok\":true}");
  }

  void StopLoad() {
    load_.stop = true;
    std::thread thread;
    {
      const std::lock_guard guard(load_.mutex);
      thread = std::move(load_.thread);
    }
    if (thread.joinable()) thread.join();
  }

  std::unique_ptr<lsm::DB> db_;
  lsm::Options options_;
  std::mutex snapshots_mutex_;
  std::map<std::uint64_t, const lsm::Snapshot*> snapshots_;
  std::uint64_t next_snapshot_ = 0;
  Load load_;
};

httplib::Server* running_server = nullptr;

void OnSignal(int) {
  if (running_server != nullptr) running_server->stop();
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::string db_path = "explorer-db";
  std::string web = LSM_EXPLORER_WEB_DIR;
  int port = 8080;
  lsm::Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto eq = arg.find('=');
    const auto name = arg.substr(0, eq);
    const std::string value(eq == std::string_view::npos ? "" : arg.substr(eq + 1));
    if (name == "--db") {
      db_path = value;
    } else if (name == "--port") {
      port = std::stoi(value);
    } else if (name == "--web") {
      web = value;
    } else if (name == "--memtable_bytes") {
      options.memtable_bytes = std::stoull(value);
    } else {
      std::cerr << "usage: lsm_server [--db=PATH] [--port=8080] [--web=DIR] "
                   "[--memtable_bytes=N]\n";
      return 2;
    }
  }
  std::unique_ptr<lsm::DB> db;
  const auto status = lsm::DB::Open(options, db_path, &db);
  if (!status.ok()) {
    std::cerr << "open " << db_path << ": " << status.ToString() << '\n';
    return 1;
  }
  httplib::Server server;
  {
    Explorer explorer(std::move(db), options);
    explorer.Register(server);
    if (!server.set_mount_point("/", web)) {
      std::cerr << "web directory not found: " << web << '\n';
      return 1;
    }
    running_server = &server;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::cout << "stratadb explorer on http://127.0.0.1:" << port << "  (db: " << db_path << ")"
              << std::endl;
    if (!server.listen("127.0.0.1", port)) {
      std::cerr << "cannot listen on port " << port << '\n';
      return 1;
    }
  }  // The explorer releases snapshots and stops the loader before the DB closes.
  running_server = nullptr;
  return 0;
}
