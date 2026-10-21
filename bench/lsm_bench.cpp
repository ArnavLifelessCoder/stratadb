// Benchmark runner for this engine and LevelDB. One invocation runs one phase
// against one engine and prints one JSON object. bench/run_benchmarks.py
// drives the phases, repetitions and result files.
//
//   lsm_bench --engine=lsm|leveldb --db=PATH --phase=PHASE [options]
//
// Phases:
//   fillseq      insert --num keys in key order into a new DB
//   fillrandom   insert --num keys in a seeded random order into a new DB
//   readrandom   --reads point reads of existing keys, uniform
//   readseq      one full ordered scan
//   readmissing  --reads point reads of keys that sit between stored keys
//   ycsb         replay --trace (see python/ycsb_driver.py)
//
// Keys are 16-byte zero-padded decimal indices, values --value_size bytes taken
// from a seeded random buffer (incompressible; compression is off anyway).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "lsm/db.hpp"
#include "table/table.hpp"

namespace {

struct Config {
  std::string engine;
  std::string db;
  std::string phase;
  std::string trace;
  std::uint64_t num = 1000000;
  std::uint64_t reads = 1000000;
  std::uint64_t key_stride = 1;
  std::size_t value_size = 100;
  std::size_t memtable_bytes = 4 << 20;
  std::size_t block_bytes = 4096;
  std::size_t cache_bytes = 64 << 20;
  int bloom_bits = 10;
  std::uint64_t seed = 24301;
  std::string cpus;  // e.g. "0-7"; empty leaves affinity alone.
};

// ---------------------------------------------------------------- engines

class Engine {
 public:
  virtual ~Engine() = default;
  virtual auto Put(std::string_view key, std::string_view value) -> bool = 0;
  // Returns false only on an error; `found` reports presence.
  virtual auto Get(std::string_view key, std::string& value, bool& found) -> bool = 0;
  // Visits up to `count` entries from `start`; returns entries visited or -1.
  virtual auto Scan(std::string_view start, std::uint64_t count) -> std::int64_t = 0;
  virtual auto ScanAll() -> std::int64_t = 0;
  virtual void Extra(std::ostream& json) = 0;
};

class LsmEngine final : public Engine {
 public:
  explicit LsmEngine(const Config& config) {
    const lsm::Options options{.memtable_bytes = config.memtable_bytes,
                               .block_bytes = config.block_bytes,
                               .bloom_bits_per_key = config.bloom_bits,
                               .block_cache_bytes = config.cache_bytes};
    const auto status = lsm::DB::Open(options, config.db, &db_);
    if (!status.ok()) {
      std::cerr << "open: " << status.ToString() << '\n';
      std::exit(1);
    }
  }
  auto Put(std::string_view key, std::string_view value) -> bool override {
    return db_->Put({}, key, value).ok();
  }
  auto Get(std::string_view key, std::string& value, bool& found) -> bool override {
    const auto status = db_->Get({}, key, &value);
    found = status.ok();
    return status.ok() || status.IsNotFound();
  }
  auto Scan(std::string_view start, std::uint64_t count) -> std::int64_t override {
    auto it = db_->NewIterator({});
    std::int64_t seen = 0;
    std::size_t bytes = 0;
    for (it->Seek(start); it->Valid() && static_cast<std::uint64_t>(seen) < count; it->Next()) {
      bytes += it->value().size();
      ++seen;
    }
    sink_ += bytes;
    return it->status().ok() ? seen : -1;
  }
  auto ScanAll() -> std::int64_t override {
    auto it = db_->NewIterator({});
    std::int64_t seen = 0;
    std::size_t bytes = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      bytes += it->key().size() + it->value().size();
      ++seen;
    }
    sink_ += bytes;
    return it->status().ok() ? seen : -1;
  }
  void Extra(std::ostream& json) override {
    const auto stats = db_->GetStats();
    json << ",\"engine_user_bytes\":" << stats.user_bytes_written
         << ",\"engine_disk_bytes\":" << stats.disk_bytes_written
         << ",\"wal_syncs\":" << stats.wal_syncs << ",\"stall_micros\":" << stats.stall_micros
         << ",\"cache_hits\":" << stats.block_cache_hits
         << ",\"cache_misses\":" << stats.block_cache_misses
         << ",\"data_blocks_read\":" << lsm::internal::table::TableReader::DataBlocksRead()
         << ",\"levels\":[";
    for (std::size_t i = 0; i < stats.level_file_counts.size(); ++i) {
      json << (i ? "," : "") << stats.level_file_counts[i];
    }
    json << "]";
  }
  void Close() { db_.reset(); }
  std::size_t sink_ = 0;

 private:
  std::unique_ptr<lsm::DB> db_;
};

class LevelDbEngine final : public Engine {
 public:
  explicit LevelDbEngine(const Config& config)
      : cache_(leveldb::NewLRUCache(config.cache_bytes)),
        filter_(config.bloom_bits > 0 ? leveldb::NewBloomFilterPolicy(config.bloom_bits)
                                      : nullptr) {
    leveldb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = config.memtable_bytes;
    options.block_size = config.block_bytes;
    options.block_cache = cache_.get();
    options.filter_policy = filter_.get();
    options.compression = leveldb::kNoCompression;
    leveldb::DB* db = nullptr;
    const auto status = leveldb::DB::Open(options, config.db, &db);
    if (!status.ok()) {
      std::cerr << "open: " << status.ToString() << '\n';
      std::exit(1);
    }
    db_.reset(db);
    // This engine checks every block it reads from a file; match that.
    read_.verify_checksums = true;
  }
  ~LevelDbEngine() override { db_.reset(); }
  auto Put(std::string_view key, std::string_view value) -> bool override {
    return db_
        ->Put({}, leveldb::Slice(key.data(), key.size()),
              leveldb::Slice(value.data(), value.size()))
        .ok();
  }
  auto Get(std::string_view key, std::string& value, bool& found) -> bool override {
    const auto status = db_->Get(read_, leveldb::Slice(key.data(), key.size()), &value);
    found = status.ok();
    return status.ok() || status.IsNotFound();
  }
  auto Scan(std::string_view start, std::uint64_t count) -> std::int64_t override {
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_));
    std::int64_t seen = 0;
    std::size_t bytes = 0;
    for (it->Seek(leveldb::Slice(start.data(), start.size()));
         it->Valid() && static_cast<std::uint64_t>(seen) < count; it->Next()) {
      bytes += it->value().size();
      ++seen;
    }
    sink_ += bytes;
    return it->status().ok() ? seen : -1;
  }
  auto ScanAll() -> std::int64_t override {
    std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_));
    std::int64_t seen = 0;
    std::size_t bytes = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      bytes += it->key().size() + it->value().size();
      ++seen;
    }
    sink_ += bytes;
    return it->status().ok() ? seen : -1;
  }
  void Extra(std::ostream& json) override {
    json << ",\"levels\":[";
    for (int level = 0; level < 7; ++level) {
      std::string value;
      db_->GetProperty("leveldb.num-files-at-level" + std::to_string(level), &value);
      json << (level ? "," : "") << (value.empty() ? "0" : value);
    }
    json << "]";
  }
  std::size_t sink_ = 0;

 private:
  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<const leveldb::FilterPolicy> filter_;
  std::unique_ptr<leveldb::DB> db_;
  leveldb::ReadOptions read_;
};

// ---------------------------------------------------------------- helpers

auto Key(std::uint64_t index) -> std::string {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%016llu", static_cast<unsigned long long>(index));
  return buffer;
}

class Values {
 public:
  Values(std::uint64_t seed, std::size_t size) : size_(size), data_(1 << 20, '\0') {
    std::mt19937_64 random(seed);
    for (auto& byte : data_) byte = static_cast<char>(random());
  }
  auto Next() -> std::string_view {
    if (position_ + size_ > data_.size()) position_ = 0;
    const std::string_view value(data_.data() + position_, size_);
    position_ += 97;  // Overlapping windows, as in db_bench.
    return value;
  }

 private:
  std::size_t size_;
  std::size_t position_ = 0;
  std::string data_;
};

// Bytes the process has written and read through the OS so far.
struct IoCounters {
  std::uint64_t read = 0;
  std::uint64_t written = 0;
};

auto ReadIoCounters() -> IoCounters {
#ifdef _WIN32
  IO_COUNTERS io{};
  if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
    return {io.ReadTransferCount, io.WriteTransferCount};
  }
  return {};
#else
  std::ifstream file("/proc/self/io");
  IoCounters counters;
  std::string name;
  std::uint64_t value = 0;
  while (file >> name >> value) {
    if (name == "rchar:") counters.read = value;
    if (name == "wchar:") counters.written = value;
  }
  return counters;
#endif
}

void PinToCpus(const std::string& spec) {
  if (spec.empty()) return;
  const auto dash = spec.find('-');
  const auto first = std::stoul(spec.substr(0, dash));
  const auto last = dash == std::string::npos ? first : std::stoul(spec.substr(dash + 1));
#ifdef _WIN32
  DWORD_PTR mask = 0;
  for (auto cpu = first; cpu <= last; ++cpu) mask |= DWORD_PTR{1} << cpu;
  SetProcessAffinityMask(GetCurrentProcess(), mask);
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  for (auto cpu = first; cpu <= last; ++cpu) CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof(set), &set);
#endif
}

class Latencies {
 public:
  explicit Latencies(std::size_t reserve) { samples_.reserve(reserve); }
  void Add(std::chrono::steady_clock::duration elapsed) {
    samples_.push_back(static_cast<std::uint32_t>(std::min<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(), UINT32_MAX)));
  }
  void Write(std::ostream& json, const char* name) {
    json << ",\"" << name << "\":{\"count\":" << samples_.size();
    if (!samples_.empty()) {
      std::sort(samples_.begin(), samples_.end());
      const auto at = [&](double q) {
        return samples_[std::min(
            samples_.size() - 1,
            static_cast<std::size_t>(q * static_cast<double>(samples_.size())))];
      };
      json << ",\"p50_ns\":" << at(0.50) << ",\"p99_ns\":" << at(0.99)
           << ",\"p999_ns\":" << at(0.999) << ",\"max_ns\":" << samples_.back();
    }
    json << "}";
  }

 private:
  std::vector<std::uint32_t> samples_;
};

[[noreturn]] void Die(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(1);
}

// ---------------------------------------------------------------- phases

struct Result {
  std::uint64_t ops = 0;
  double seconds = 0;
  std::uint64_t user_bytes = 0;
  std::uint64_t found = 0;
};

auto Fill(Engine& engine, const Config& config, bool random_order, Latencies& latencies) -> Result {
  std::vector<std::uint64_t> order(config.num);
  std::iota(order.begin(), order.end(), 0);
  if (random_order) std::shuffle(order.begin(), order.end(), std::mt19937_64(config.seed));
  Values values(config.seed + 1, config.value_size);
  Result result;
  const auto began = std::chrono::steady_clock::now();
  for (const auto index : order) {
    const auto key = Key(index * config.key_stride);
    const auto value = values.Next();
    const auto start = std::chrono::steady_clock::now();
    if (!engine.Put(key, value)) Die("put failed");
    latencies.Add(std::chrono::steady_clock::now() - start);
    result.user_bytes += key.size() + value.size();
  }
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
  result.ops = config.num;
  return result;
}

auto ReadPoints(Engine& engine, const Config& config, bool missing, Latencies& latencies)
    -> Result {
  std::mt19937_64 random(config.seed + 2);
  Result result;
  std::string value;
  const auto began = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < config.reads; ++i) {
    // Missing keys fall between stored keys (needs --key_stride >= 2), so the
    // per-table key range does not filter them and only the bloom filter can.
    const auto index = (random() % config.num) * config.key_stride + (missing ? 1 : 0);
    const auto key = Key(index);
    bool found = false;
    const auto start = std::chrono::steady_clock::now();
    if (!engine.Get(key, value, found)) Die("get failed");
    latencies.Add(std::chrono::steady_clock::now() - start);
    result.found += found ? 1 : 0;
  }
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
  result.ops = config.reads;
  return result;
}

struct TraceOp {
  char type;
  std::uint64_t key;
  std::uint16_t length;
};

auto LoadTrace(const std::string& path, std::uint64_t& records) -> std::vector<TraceOp> {
  std::ifstream file(path, std::ios::binary);
  char magic[8];
  std::uint64_t count = 0;
  file.read(magic, 8);
  file.read(reinterpret_cast<char*>(&count), 8);
  file.read(reinterpret_cast<char*>(&records), 8);
  if (!file || std::string_view(magic, 8) != "YCSBTRC1") Die("bad trace " + path);
  std::vector<TraceOp> ops(count);
  for (auto& op : ops) {
    file.read(&op.type, 1);
    file.read(reinterpret_cast<char*>(&op.key), 8);
    file.read(reinterpret_cast<char*>(&op.length), 2);
  }
  if (!file) Die("short trace " + path);
  return ops;
}

void Ycsb(Engine& engine, const Config& config, std::ostream& json) {
  std::uint64_t records = 0;
  const auto ops = LoadTrace(config.trace, records);
  Values values(config.seed + 3, config.value_size);
  std::map<char, Latencies> latencies;
  for (const char type : {'R', 'U', 'I', 'S', 'M'}) latencies.emplace(type, Latencies(ops.size()));
  Latencies all(ops.size());
  std::string value;
  std::uint64_t found = 0;
  std::uint64_t scanned = 0;
  std::uint64_t user_bytes = 0;
  const auto began = std::chrono::steady_clock::now();
  for (const auto& op : ops) {
    const auto key = Key(op.key);
    const auto start = std::chrono::steady_clock::now();
    bool hit = false;
    switch (op.type) {
      case 'R':
        if (!engine.Get(key, value, hit)) Die("get failed");
        break;
      case 'U':
      case 'I': {
        const auto v = values.Next();
        if (!engine.Put(key, v)) Die("put failed");
        user_bytes += key.size() + v.size();
        hit = true;
        break;
      }
      case 'S': {
        const auto seen = engine.Scan(key, op.length);
        if (seen < 0) Die("scan failed");
        scanned += static_cast<std::uint64_t>(seen);
        hit = seen > 0;
        break;
      }
      case 'M': {
        if (!engine.Get(key, value, hit)) Die("get failed");
        const auto v = values.Next();
        if (!engine.Put(key, v)) Die("put failed");
        user_bytes += key.size() + v.size();
        break;
      }
      default:
        Die("bad trace op");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    latencies.at(op.type).Add(elapsed);
    all.Add(elapsed);
    found += hit ? 1 : 0;
  }
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
  json << ",\"ops\":" << ops.size() << ",\"seconds\":" << seconds
       << ",\"ops_per_sec\":" << static_cast<double>(ops.size()) / seconds
       << ",\"records\":" << records << ",\"found\":" << found << ",\"scanned\":" << scanned
       << ",\"user_bytes\":" << user_bytes;
  all.Write(json, "latency");
  for (auto& [type, samples] : latencies) {
    const std::string name = std::string("latency_") + type;
    samples.Write(json, name.c_str());
  }
}

auto ParseArgs(int argc, char** argv) -> Config {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto eq = arg.find('=');
    if (!arg.starts_with("--") || eq == std::string_view::npos) Die("bad argument");
    const auto name = arg.substr(2, eq - 2);
    const std::string value(arg.substr(eq + 1));
    if (name == "engine")
      config.engine = value;
    else if (name == "db")
      config.db = value;
    else if (name == "phase")
      config.phase = value;
    else if (name == "trace")
      config.trace = value;
    else if (name == "num")
      config.num = std::stoull(value);
    else if (name == "reads")
      config.reads = std::stoull(value);
    else if (name == "key_stride")
      config.key_stride = std::stoull(value);
    else if (name == "value_size")
      config.value_size = std::stoull(value);
    else if (name == "memtable_bytes")
      config.memtable_bytes = std::stoull(value);
    else if (name == "block_bytes")
      config.block_bytes = std::stoull(value);
    else if (name == "cache_bytes")
      config.cache_bytes = std::stoull(value);
    else if (name == "bloom_bits")
      config.bloom_bits = std::stoi(value);
    else if (name == "seed")
      config.seed = std::stoull(value);
    else if (name == "cpus")
      config.cpus = value;
    else
      Die("unknown option " + std::string(name));
  }
  if ((config.engine != "lsm" && config.engine != "leveldb") || config.db.empty() ||
      config.phase.empty()) {
    Die("usage: lsm_bench --engine=lsm|leveldb --db=PATH --phase=PHASE [options]");
  }
  return config;
}
}  // namespace

auto main(int argc, char** argv) -> int {
  const auto config = ParseArgs(argc, argv);
  PinToCpus(config.cpus);
  const bool fresh = config.phase == "fillseq" || config.phase == "fillrandom";
  if (fresh) {
    std::filesystem::remove_all(config.db);
  }
  const auto io_before = ReadIoCounters();
  const auto opened = std::chrono::steady_clock::now();
  std::unique_ptr<Engine> engine;
  if (config.engine == "lsm") {
    engine = std::make_unique<LsmEngine>(config);
  } else {
    engine = std::make_unique<LevelDbEngine>(config);
  }
  const auto open_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - opened).count();
  std::ostringstream json;
  json << "{\"engine\":\"" << config.engine << "\",\"phase\":\"" << config.phase
       << "\",\"num\":" << config.num << ",\"value_size\":" << config.value_size
       << ",\"key_size\":16,\"bloom_bits\":" << config.bloom_bits
       << ",\"cache_bytes\":" << config.cache_bytes << ",\"block_bytes\":" << config.block_bytes
       << ",\"memtable_bytes\":" << config.memtable_bytes << ",\"seed\":" << config.seed
       << ",\"open_seconds\":" << open_seconds;
  Latencies latencies(std::max(config.num, config.reads));
  Result result;
  if (fresh) {
    result = Fill(*engine, config, config.phase == "fillrandom", latencies);
  } else if (config.phase == "readrandom" || config.phase == "readmissing") {
    const auto blocks_before = lsm::internal::table::TableReader::DataBlocksRead();
    result = ReadPoints(*engine, config, config.phase == "readmissing", latencies);
    if (config.engine == "lsm") {
      json << ",\"data_blocks_read_per_lookup\":"
           << static_cast<double>(lsm::internal::table::TableReader::DataBlocksRead() -
                                  blocks_before) /
                  static_cast<double>(result.ops);
    }
  } else if (config.phase == "readseq") {
    const auto began = std::chrono::steady_clock::now();
    const auto seen = engine->ScanAll();
    if (seen < 0) Die("scan failed");
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    result.ops = static_cast<std::uint64_t>(seen);
  } else if (config.phase == "ycsb") {
    Ycsb(*engine, config, json);
  } else {
    Die("unknown phase " + config.phase);
  }
  if (config.phase != "ycsb") {
    json << ",\"ops\":" << result.ops << ",\"seconds\":" << result.seconds
         << ",\"ops_per_sec\":" << static_cast<double>(result.ops) / result.seconds
         << ",\"user_bytes\":" << result.user_bytes << ",\"found\":" << result.found;
    if (config.phase != "readseq") latencies.Write(json, "latency");
  }
  // OS-level bytes written while the phase ran, before closing: same method for
  // both engines. Background work still queued at this point is not included.
  const auto io_end = ReadIoCounters();
  json << ",\"os_bytes_written\":" << io_end.written - io_before.written
       << ",\"os_bytes_read\":" << io_end.read - io_before.read;
  engine->Extra(json);
  const auto closing = std::chrono::steady_clock::now();
  engine.reset();
  const auto io_closed = ReadIoCounters();
  json << ",\"close_seconds\":"
       << std::chrono::duration<double>(std::chrono::steady_clock::now() - closing).count()
       << ",\"os_bytes_written_through_close\":" << io_closed.written - io_before.written << "}";
  std::cout << json.str() << std::endl;
  return 0;
}
