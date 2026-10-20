# M2 review packet

M2 implements the durable in-memory DB on branch `m2`, branched from
`main` at `4c3890c`. M3 has not started.

All six hosted jobs passed on the implementation commit
`9cbe618f51da4ae4cb8252be8235943147fac3e3`:
[CI run 39427006423](https://github.com/ArnavLifelessCoder/stratadb/actions/runs/39427006423).
Subsequent packet and compiler-metadata commits do not change storage code.
The branch's latest run is also checked before handing this packet over.

## Built

- Public `DB` operations: Open, Put, Delete, atomic WriteBatch, Get, GetStats.
  Paths use `std::filesystem::path`. The owner chose to add snapshot and iterator
  methods in M5 instead of exposing undefined methods now.
- Arena-backed skiplist, 12 levels, promotion probability 1/4, descending
  sequence order, tombstones, one writer and concurrent readers with atomic
  acquire/release links.
- WAL with CRC32C over length/type/payload, canonical batch encoding, contiguous
  56-bit sequence validation, native sync before acknowledging sync writes,
  replay, durable tail truncation, and corruption errors for damaged middle
  records. Write/sync failures stop subsequent writes.
- Windows/POSIX file implementations, exclusive database locks, synced temporary
  WAL creation and atomic rename. POSIX also syncs directory entries.
- Runtime SSE4.2 CRC32C dispatch with a portable table fallback. Both paths run
  RFC 3720 B.4 vectors and randomized comparisons with the bitwise reference.
- CI clang-tidy using the repository configuration. Windows uses MSVC cl.
  All JavaScript actions now use Node 24; MSVC environment discovery is a local
  PowerShell script. Linux is pinned to Ubuntu 24.04 and configure steps are merged.
- Historical M1 packets moved to [results/reviews](../reviews/m1-review.md).

Contracts, format, error handling and limits are described in
[the storage design](../../docs/memtable-wal.md).
See [building and reproduction](../../docs/building.md).

## Files changed

The [complete inventory](files.txt) lists changes relative to `4c3890c`,
including result files. Main groups:

| Area | Files |
| --- | --- |
| API | `include/lsm/db.hpp`, `include/lsm/write_batch.hpp` |
| Storage | `src/db.cpp`, `src/memtable.*`, `src/wal.*`, `src/write_batch.*` |
| Native I/O | `src/platform/file.hpp`, `windows.cpp`, `posix.cpp` |
| CRC and tidy | `src/util/crc32c.*`, existing utility headers/sources, `.clang-tidy` |
| Tests | batch, memtable, platform, WAL and DB GoogleTests; process helper and Python kill harness |
| Build/checks | CMake, CI, PowerShell environment and metadata scripts, clang-tidy script |
| Documentation | `docs/building.md`, `docs/memtable-wal.md`, historical review relocation |
| Evidence | `results/m2/`, including full logs and platform-tagged JSON |

## Full local test output

Local toolchain: VS 2022 MSVC 19.44.35229.0, CMake 3.31.6-msvc6, Ninja 1.12.1,
Windows 11 x64. [Machine metadata](windows.json).

| Check | Result | Complete output |
| --- | --- | --- |
| Debug | 46/46 GoogleTests, 5/5 CTest entries | [windows-debug.log](windows-debug.log) |
| MSVC ASan | 46/46 GoogleTests, 5/5 CTest entries | [windows-asan.log](windows-asan.log) |
| Debug process kills | 12/12 cycles, six sync and six async | [log](crash-Windows-debug.log), [JSON](crash-Windows-debug.json) |
| ASan process kills | 12/12 cycles, six sync and six async | [log](crash-Windows-asan.log), [JSON](crash-Windows-asan.json) |
| x64 clang-tidy | Pass, no unsuppressed project findings | [tidy-Windows.log](tidy-Windows.log) |
| Formatting, Python lint, hash oracle | Pass | [lint-Windows.log](lint-Windows.log) |
| Compiler metadata regression | Pass | [metadata-after.log](metadata-after.log) |

Clang-tidy reports suppressed diagnostics from the Windows SDK and standard
library. One targeted project suppression preserves StatusCode's M1 `int`
representation. The configured checks remain enabled, including warnings as
errors. The bundled x86 clang-tidy executable crashed; the
[diagnostic log](tidy-Windows-x86-tool-crash.log) is retained, and x64 clang-tidy
completed successfully. CI independently uses clang-tidy 18 on Linux.

Component runs are also retained: [CRC Debug](crc-debug.log),
[CRC ASan](crc-asan.log), [batch](batch-debug.log),
[memtable Debug](memtable-debug.log), [memtable/batch ASan](memtable-batch-asan.log),
[platform](platform-debug.log), [WAL](wal-debug.log), and [DB](db-debug.log).

## Full hosted test output

The native jobs each ran all 46 GoogleTests and all five CTest entries.
Linux uses GCC 13.3; Windows uses MSVC 19.51.36260.0 from its installed runner
image. No compiler, ASan, UBSan or TSan findings remain.

| Job | Result | Complete output |
| --- | --- | --- |
| Ubuntu 24.04 Debug | Pass | [log](ci-39427006423/118641984842.log) |
| Ubuntu 24.04 ASan/UBSan | Pass | [log](ci-39427006423/118641984715.log) |
| Ubuntu 24.04 TSan | Pass, including concurrent readers/writers | [log](ci-39427006423/118641984722.log) |
| Windows MSVC Debug | Pass | [log](ci-39427006423/118641984777.log) |
| Windows MSVC ASan | Pass | [log](ci-39427006423/118641984533.log) |
| Lint and clang-tidy | Pass | [log](ci-39427006423/118641984395.log) |

[Machine-readable job summary](ci-39427006423/summary.json).
Each Debug/ASan job also passed 12 process-kill cycles:
[Linux Debug](ci-39427006423/crash-Linux-debug.json),
[Linux ASan](ci-39427006423/crash-Linux-asan.json),
[Windows Debug](ci-39427006423/crash-Windows-debug.json),
[Windows ASan](ci-39427006423/crash-Windows-asan.json).
These JSON files are extracted from the corresponding complete job logs.

## Acceptance criteria

| Criterion | Evidence | Status |
| --- | --- | --- |
| Durable in-memory DB without SSTables | Public API, native WAL sync, locks and recovery tests | Met |
| Random operation streams against std::map, 10k seeds | 10,000 seeds × 64 operations on every full suite run; all possible keys checked after each stream | Met |
| Clean-close WAL replay gives identical state | 100 grouped DB close/reopen checks per full run, plus post-replay append/reopen checks and large/binary-value tests | Met |
| Torn final record handled | Every final-record truncation point, final bad CRC, on-disk short tail and kill/reopen tests | Met |
| Mid-file corruption rejected | A flipped byte at every position of a middle record, including damaged lengths; failed Open does not truncate it | Met |
| Debug and ASan locally | Complete passing outputs above | Met |
| Threaded code passes Linux TSan | Full hosted suite, including four readers and four DB writers | Met |
| SSE4.2 and table CRC paths match RFC and bitwise reference | Both parameterized paths passed; no skipped SSE4.2 tests on tested x64 machines | Met |
| Run clang-tidy with repo config | Local x64 and hosted Linux checks passed | Met |

## Bugs found and retained regressions

- Skiplist lookup initially reloaded the final predecessor link and could return
  a version newer than the reader's sequence. The concurrent reader test failed
  before the fix ([output](memtable-regression-before.log)); lookup now returns
  its already-compared candidate, and the same test passes under all three
  sanitizer/build configurations.
- Initial hosted clang-tidy rejected a POSIX reverse-iterator loop
  ([output](ci-tidy-before.log)). It now uses `std::views::reverse`, and the
  hosted lint job passes.
- Compiler metadata initially captured `cl` usage output due to stdout/stderr
  ordering ([failing regression](metadata-before.log)). It now reads the
  executable's numeric file version; the PowerShell regression remains in tests.

## Limits and next gate

M2 keeps all versions in memory and one WAL; memtable sizing becomes active with
M4 flushing. Group commit is M7. Snapshots and iterators are M5. The 12-cycle
process-kill smoke tests do not claim the M6 500-cycle acceptance criterion.
There are no benchmark claims or LevelDB builds.

The WAL has no independent record directory. Some ambiguous multi-byte damage
cannot be distinguished from a torn tail; the recovery rules are documented in
the storage design. OS sync/rename guarantees remain dependent on the underlying
filesystem and storage device.

Await owner approval before starting M3 or merging M2 into main.
