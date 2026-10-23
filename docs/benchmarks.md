# Benchmarks

The numbers in the README come from one run of `bench/run_benchmarks.py`,
recorded in `results/m8/bench/`:

* `bench-raw.json`: every phase of every repetition, plus machine and settings;
* `bench-summary.json`, `bench-summary.md`: medians and min/max, made by
  `bench/summarize.py` from the raw file;
* `bench-summary-throughput.png`: the same medians as a chart;
* `bench.log`: the console output of the run.

## What was measured

Both engines ran in the same process-per-phase runner (`bench/lsm_bench.cpp`),
which links this engine and LevelDB and drives either through the same calls:
same keys (16-byte zero-padded decimal indices), same 100-byte values from one
seeded random buffer, same operation order and seeds, single-threaded.

| Phase | What it does |
|---|---|
| fillseq | 10M puts in key order into a new DB |
| fillrandom | 10M puts in a seeded random order into a new DB, then reused below |
| readrandom | 1M uniform point reads of existing keys |
| readseq | one full ordered scan |
| ycsb-a..f | 1M operations each, traces from `python/ycsb_driver.py` (scrambled zipfian, theta 0.99; D uses "latest"; E scans 1 to 100 records), replayed by the C++ runner in YCSB's order A, B, C, F, D, E |
| bloom10/bloom0 | 2M keys at every other index, then 1M reads of the missing indices between them, block cache off |

Each engine ran 5 repetitions; the engine order alternated between repetitions.
Throughput is operations divided by the time of the phase's loop. Latency is
measured per operation; the summary reports the median across repetitions of
each repetition's p50 and p99.

Write amplification is measured two ways. The engine-independent one is the
bytes the process wrote through the OS (`GetProcessIoCounters`) during the
load, divided by the user bytes (key plus value). It is reported at the end of
the load and after closing the DB, because this engine finishes pending
compactions on close and LevelDB does not. The second is this engine's own
`GetStats()` (disk bytes over user bytes).

## Settings

| Setting | Both engines |
|---|---|
| Memtable / write buffer | 4 MiB |
| Block size | 4 KiB |
| Block cache | 64 MiB LRU (0 for the missing-key test) |
| Bloom filter | 10 bits per key (0 for bloom0) |
| Compression | none: LevelDB built without Snappy, `kNoCompression` set |
| Sync writes | off |
| Checksums on read | both verify each block read from a file (`verify_checksums = true` for LevelDB) |
| L0 trigger / slowdown / stop | 4 / 8 / 12 |
| Table file target | 2 MiB |
| Level sizes | L1 10 MiB, 10x per level |

LevelDB is release 1.23 (commit 99b3c03b3284f5886f9ef9a4ef703d57373e61be),
built from source by CMake with the same compiler and flags (MSVC, `/O2 /Ob2
/DNDEBUG`). It was built without the optional crc32c and tcmalloc libraries,
so it computes CRC32C in portable software while this engine uses SSE4.2.

## Machine and caveats

AMD Ryzen 9 8940HX (16 cores, 32 threads), 15 GiB RAM, Samsung NVMe SSD (MZVL81T0HFLB), Windows 11
Home build 26300, MSVC 19.44.35229. The exact values are in the raw file's `machine`
field. The engine code measured is commit 2d48b85, built from `git archive`.

* Windows has no `drop_caches` equivalent without extra admin tools, so the OS
  file cache was not cleared between phases. The 10M-key data set is about
  1.2 GB and fits in RAM, so reads are in-memory reads.
* This is a laptop: CPU boost stayed on and threads were not pinned. The
  min/max spread in the tables shows the run-to-run noise.

## Reading the results

* **Point reads and missing keys are faster here.** Reads check the memtables
  and then each level with one table per level, and cached blocks skip the CRC.
  With bloom filters the missing-key test reads 0.014 data blocks per lookup.
* **Sequential load writes about twice as much.** LevelDB moves a table that
  overlaps nothing in the next level down without rewriting it ("trivial
  move"). This engine always merges, so already-sorted data is rewritten once
  per level. Random load, where trivial moves rarely apply, is close (10.4 vs
  9.5).
* **Loads are about half as fast.** Besides the extra rewriting above, one
  background thread does both flushes and compactions, and a compaction's
  output is not split by overlap with the level below (LevelDB's grandparent
  rule), so some compactions are larger and stall writers longer. The wide
  fillrandom range comes from how often a run hit those stalls.
* **Scans are slower.** Each step goes through the merging iterator with key
  validation at every level and virtual calls per child; LevelDB's iterators
  do less checking per entry. Short scans (YCSB E) also pay for building the
  iterator stack for each scan.
