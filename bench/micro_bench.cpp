// Google Benchmark microbenchmarks for hot building blocks. The engine-level
// comparison with LevelDB is bench/lsm_bench.cpp.

#include <benchmark/benchmark.h>

#include <random>
#include <string>
#include <vector>

#include "memtable.hpp"
#include "table/block.hpp"
#include "table/bloom.hpp"
#include "table/format.hpp"
#include "util/crc32c.hpp"

namespace lsm::internal {
namespace {
auto RandomBytes(std::size_t size) -> std::string {
  std::mt19937_64 random(24301);
  std::string bytes(size, '\0');
  for (auto& byte : bytes) byte = static_cast<char>(random());
  return bytes;
}

void BM_Crc32cTable(benchmark::State& state) {
  const auto bytes = RandomBytes(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(ExtendCrc32cTable(0, bytes));
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_Crc32cTable)->Arg(4096);

void BM_Crc32cSse42(benchmark::State& state) {
  if (!HasSse42Crc32c()) {
    state.SkipWithError("SSE4.2 unavailable");
    return;
  }
  const auto bytes = RandomBytes(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(ExtendCrc32cSse42(0, bytes));
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_Crc32cSse42)->Arg(4096);

void BM_BloomMayContain(benchmark::State& state) {
  std::vector<std::string> keys;
  for (int i = 0; i < 100000; ++i) keys.push_back("key-" + std::to_string(i));
  std::string encoded;
  if (!table::Bloom::Build(keys, 10, encoded).ok()) state.SkipWithError("build failed");
  table::Bloom bloom;
  if (!table::Bloom::Open(encoded, bloom).ok()) state.SkipWithError("open failed");
  std::size_t i = 0;
  for (auto _ : state) benchmark::DoNotOptimize(bloom.MayContain(keys[i++ % keys.size()]));
}
BENCHMARK(BM_BloomMayContain);

void BM_SeekBlock(benchmark::State& state) {
  std::vector<table::Entry> entries;
  for (int i = 0; i < 64; ++i) {
    auto key = std::to_string(1000000 + i);
    entries.push_back(
        {.key = table::InternalKey(key, 1, WriteBatch::Type::kPut), .value = std::string(48, 'v')});
  }
  std::string block;
  if (!table::EncodeBlock(entries, block).ok()) state.SkipWithError("encode failed");
  std::size_t i = 0;
  table::Entry found;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        table::SeekBlock(block, entries[i++ % entries.size()].key, found).ok());
  }
}
BENCHMARK(BM_SeekBlock);

void BM_MemTableInsert(benchmark::State& state) {
  std::uint64_t sequence = 0;
  auto table = std::make_unique<MemTable>();
  std::mt19937_64 random(24301);
  for (auto _ : state) {
    if (table->MemoryUsage() > (64 << 20)) {
      state.PauseTiming();
      table = std::make_unique<MemTable>();
      state.ResumeTiming();
    }
    const auto key = std::to_string(random());
    benchmark::DoNotOptimize(table->Insert(++sequence, WriteBatch::Type::kPut, key, "value"));
  }
}
BENCHMARK(BM_MemTableInsert);
}  // namespace
}  // namespace lsm::internal

BENCHMARK_MAIN();
