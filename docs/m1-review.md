# M1 review packet

Date: 20 October 2026 (Asia/Calcutta).

Review update: **M1 remains open** until every hosted CI job passes.
See [the CI follow-up](m1-ci-review.md) for actual Linux results, the Windows
bootstrap failure, and the prepared workflow fix. The original local-only
verification packet below is retained as historical evidence.

M1 implementation and local acceptance tests are complete. Work is on local
`main`, with remote `https://github.com/ArnavLifelessCoder/stratadb.git`.
Nothing has been pushed. M2 has not started.

## What was built

- C++20 static library with CMake/Ninja Debug, Release, ASan and TSan presets.
  Project code and tests compile with strict warnings treated as errors.
- Windows/Linux CI for Debug and ASan, a separate Ubuntu TSan job, and formatting,
  Python lint and hash-oracle checks. Dependencies use pinned FetchContent commits.
- An owned-message `Status`, non-owning `Slice` alias, unsigned byte comparison,
  common-prefix length and transactional prefix consumption.
- Table-driven CRC32C with incremental extension and a bitwise test reference.
- Portable MurmurHash3 x64_128 low-64-bit hashing with independent oracle vectors.
- Canonical unsigned 32-bit and 64-bit varints with checked, transactional decoding.
- Seeded tests, build helpers, fixture verification, full test logs and platform
  metadata.

The utility contracts are documented in [utilities.md](utilities.md).
Build and reproduction commands are in [building.md](building.md).
The DB interface has not been introduced in M1.

## Acceptance criteria

| Criterion | Result | Evidence |
|---|---|---|
| CMake, CI, Status, string-view helpers, CRC32C, hash, varints | Implemented | Source inventory below; CI execution remains pending |
| CRC32C matches RFC 3720 Appendix B.4 | Met locally: all five vectors pass | `Crc32cTest.Rfc3720AppendixB4Vectors` |
| Varint round-trip fuzz test | Met locally for both integer widths | 200,000 round trips per full suite invocation |
| All tests pass under Debug and ASan | Met on native Windows/MSVC | 26 GoogleTests and all five CTest entries pass in each preset |
| Tests after each component | Met | Individual component logs linked below |
| Reproducible randomized tests | Met | Default seed 24301 and replay seed 42 pass; parser checks also pass |
| Formatting and Python lint | Met | clang-format, Ruff check/format, independent hash fixture check |
| TSan once project threads exist | Not applicable to M1 | M1 has no project threads; Ubuntu job is configured |

Extra coverage includes 1,024 CRC differential inputs per seed, CRC split points
and bit flips, 132 hash oracle vectors at eight alignments, all integer bit
boundaries, truncated varints, overflow, redundant encodings, and 100,000 arbitrary
byte strings per full suite invocation checked against an independent decoder.

## Full output and environment

These are complete captured outputs, not excerpts:

| Run | Result | Full output |
|---|---|---|
| Windows Debug, seed 24301 | 26/26 GoogleTests; 5/5 CTest entries | [windows-debug.log](../results/m1/windows-debug.log) |
| Windows MSVC ASan, seed 24301 | 26/26 GoogleTests; 5/5 CTest entries | [windows-asan.log](../results/m1/windows-asan.log) |
| Windows Debug, seed 42 | 26/26 GoogleTests | [windows-debug-seed42.log](../results/m1/windows-debug-seed42.log) |
| Windows MSVC ASan, seed 42 | 26/26 GoogleTests | [windows-asan-seed42.log](../results/m1/windows-asan-seed42.log) |
| Formatting, lint, oracle regeneration check | Pass | [lint.log](../results/m1/lint.log) |

The five CTest entries are the complete GoogleTest suite, a maximum-value seed
argument check, and rejection checks for empty, nonnumeric and overflowing seeds.
No compiler warnings or sanitizer findings were reported in successful runs.

Environment: Windows 11 Home x64, MSVC 19.44.35229, CMake 3.31.6-msvc6, Ninja
1.12.1. See [windows.json](../results/m1/windows.json) for the recorded machine,
timestamp and tested source commit. All project translation units were checked
for `/W4 /WX`; all ASan translation units include `/fsanitize=address`, with
`/RTC1` absent.

Component-by-component outputs:

- [Status](../results/m1/components/status.log)
- [String views](../results/m1/components/slice.log)
- [CRC32C](../results/m1/components/crc32c.log)
- [Hash](../results/m1/components/hash.log)
- [Varints](../results/m1/components/varint.log)

The first ASan configure attempt encountered GitHub connection resets and DNS
failures while fetching GoogleTest. Its
[failure log](../results/m1/windows-asan-fetch-failure.log) is retained.
The successful ASan build reused the already-fetched source at the exact pinned
commit `52eb8108c5bdec04579160ae17225d66034bd723` and compiled separate instrumented
objects. See the [cache configuration log](../results/m1/asan-cached-dependency.log).
The local cache uses `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` pointing at the Debug
source checkout and `FETCHCONTENT_UPDATES_DISCONNECTED=ON`; these overrides are
in ignored build caches. Committed presets still fetch from the pinned repository.

## Files changed

All files in this milestone are additions to the initially empty repository.
The [complete file inventory](../results/m1/files.txt) includes every tracked
source, script, configuration file, document and result.

| Area | Files |
|---|---|
| Build and style | `CMakeLists.txt`, `CMakePresets.json`, `.clang-format`, `.clang-tidy`, `.gitattributes`, `.gitignore`, `pyproject.toml`, `requirements-dev.txt` |
| CI | `.github/workflows/ci.yml` |
| Public utilities | `include/lsm/status.hpp`, `include/lsm/slice.hpp` |
| Implementations | `src/status.cpp`, `src/util/crc32c.{hpp,cpp}`, `src/util/hash.{hpp,cpp}`, `src/util/varint.{hpp,cpp}` |
| Tests | `tests/test_main.cpp`, `tests/test_support.hpp`, `tests/status_test.cpp`, `tests/slice_test.cpp`, `tests/crc32c_test.cpp`, `tests/hash_test.cpp`, `tests/hash_vectors.hpp`, `tests/varint_test.cpp` |
| Scripts | `scripts/build.ps1`, `scripts/verify.ps1`, `scripts/check_format.py`, `scripts/hash_vectors.py` |
| Documentation | `docs/building.md`, `docs/utilities.md`, `docs/m1-review.md` |
| Results | `results/m1/` logs, machine metadata and file inventory |
| Layout placeholders | `.gitkeep` in `bench/`, `python/`, `src/platform/`, `results/` |

The spec is unchanged and ignored. Build outputs and the local virtual environment
are ignored. No LevelDB build or benchmark has been performed.

## Local commits

The tested implementation ends at `0b6b7cd`. The following commit only adds this
packet and captured verification artifacts.

| Commit | Logical change |
|---|---|
| `604179a` | Build skeleton and tested Status API |
| `a10d4e3` | Binary string-view helpers |
| `fb5cdfd` | CRC32C and reference tests |
| `53f4fe8` | MurmurHash3 and independent oracle vectors |
| `ae7a672` | Varints and seeded fuzz tests |
| `0b6b7cd` | Seed parser checks and utility documentation |

## Remaining gate

Hosted Windows/Linux CI and Ubuntu TSan have not run because pushing is awaiting
owner approval. Linux compiler and sanitizer results are therefore unverified.
The Release preset is configured but has not been exercised; benchmarks remain
scheduled for M8. M1 contains no durability or storage implementation.

Awaiting `approved` before pushing `main` or beginning M2.
