# M5 review packet

Implementation commit: `ca581d60c1f1563cbeed5576c8b7c1253799bc9a` on `m5`.
Approved M4 was fast-forwarded into `main` at `49f92f18995115bc40b0261f40d36dcc1ef6fd1d`
and pushed. M6 has not started.

## Built

- L0 compaction across all current L0 files and overlapping L1 files; higher-level
  size scoring and round-robin selection through L6.
- K-way merge with snapshot-aware version retention and conservative deeper-level
  tombstone retention. Outputs use the existing CRC/sync/atomic-rename protocol
  and one durable manifest edit.
- Non-overlapping higher-level range indexes and binary-search point reads.
- Sequence snapshots that retain their memtables and Version, plus stable ordered
  iterators with Seek, Next and Prev. The public API uses the owner-approved
  LevelDB-style interface and requires views to be released before DB destruction.
- Reference-based obsolete-table reclamation and startup cleanup of unreferenced
  storage files.

The protocol, locking, API contracts and memory limits are described in
[compaction-snapshots.md](../../docs/compaction-snapshots.md).

## M4 review follow-ups

| Request | Resolution |
| --- | --- |
| Do not reject an already-applied batch | Trailing rotation can record a sticky failure, but Write returns OK once WAL append and memtable publication succeeded. Later writes return the failure. |
| Obsolete-WAL delete failure must not poison writes | Failure is logged and ignored by the write path; successful Open retries cleanup. |
| Startup cleanup | After recovery and before the worker starts, remove unreferenced numbered WALs/tables, recognized storage temp files and the unused legacy WAL; preserve live and unrelated names. |
| Move Apply I/O outside the DB mutex | Separate writer/manifest serialization; reader opening and manifest append/sync run without the DB mutex. VersionSet publishes immutable state atomically after sync. |
| Quick tests, retain 10k gate | `--quick` or `build.ps1 -Quick` runs all tests with 100 DB differential seeds. Default CTest, CI and milestone verification retain all 10,000 seeds. |
| Explain FILE_SHARE_WRITE | Mapping remains read-only; sharing permits independent integrity-test handles, matching POSIX mmap's lack of exclusive write locking. Engine tables remain immutable. |
| Windows mapping deletion test | Native deletion-attempt test retains readable mapped bytes, then verifies reclamation after close; DB integration verifies that real obsolete SSTables stay until snapshots and iterators release their versions. |

## Files changed

[files.txt](files.txt) contains the complete inventory relative to accepted M4.

| Area | Files |
| --- | --- |
| Public API | include/lsm/db.hpp, include/lsm/iterator.hpp |
| Compaction and iterator components | src/compaction.hpp, compaction.cpp, iterator.hpp, iterator.cpp |
| DB and version integration | src/db.cpp, src/version_set.hpp, version_set.cpp, src/memtable.hpp |
| Cleanup and mapping | src/file_cleanup.hpp, file_cleanup.cpp, src/platform/mapping_windows.cpp |
| Tests | tests/compaction_test.cpp, iterator_test.cpp, snapshot_test.cpp, cleanup_test.cpp, version_set_test.cpp, mapping_test.cpp, flush_test.cpp, db_test.cpp, test_main.cpp, test_support.hpp |
| Harnesses | CMakeLists.txt, scripts/build.ps1, scripts/verify.ps1, tests/crash_smoke.py |
| Documentation and evidence | docs/compaction-snapshots.md, building.md, flush-recovery.md, sstable.md, follow-ups.md, results/m5/ |

## Component and regression evidence

- [write-status-before.log](write-status-before.log) reproduces the applied-write
  error before the fix; [write-status-debug.log](write-status-debug.log) passes
  all ten flush tests afterward.
- [cleanup-before.log](cleanup-before.log) reproduces poisoned writes and leftover
  orphan files; [cleanup-debug.log](cleanup-debug.log) passes both regressions
  and the flush tests afterward.
- [mapping-debug.log](mapping-debug.log) records native mapped deletion and the
  after-open corruption check.
- [manifest-publication-debug.log](manifest-publication-debug.log) includes
  blocked-sync state visibility, concurrent publication and DB regressions.
- [level-index-debug.log](level-index-debug.log) checks key ordering and rejection
  of overlapping higher-level ranges.
- [compaction-unit-debug.log](compaction-unit-debug.log) compares heap merging
  with full sorting and supported snapshot views with a map reference.
- [iterator-unit-debug.log](iterator-unit-debug.log) covers seek, forward/reverse
  traversal, empty/binary keys, errors and retained Version ownership.
- [db-integration-debug.log](db-integration-debug.log) and
  [snapshot-final-debug.log](snapshot-final-debug.log) cover map equivalence,
  bounded range scans, version reclamation, deeper-level tombstones, failed
  compaction publication and concurrent snapshot readers.
- [random-snapshots-debug.log](random-snapshots-debug.log) passes the six
  snapshot cases with snapshot capture points driven by the printed test seed.
- [integration-quick-debug.log](integration-quick-debug.log) is the development
  run of all 105 tests; it is explicitly not the 10,000-seed milestone gate.
- [seed-cli-debug.log](seed-cli-debug.log) checks seed parsing. Invalid-seed
  process timeouts now allow 60 seconds after the recorded M4 ASan exit timeout;
  the expected parser diagnostic remains mandatory.
- [tidy-before.log](tidy-before.log) records a missing nodiscard annotation;
  [tidy-Windows.log](tidy-Windows.log) passes after the fix.
- [lint-Windows.log](lint-Windows.log) records formatting, Ruff and hash-oracle checks.
- [ci-reclamation-before.log](ci-reclamation-before.log) reproduces a Windows
  attribute-query exception while an obsolete file's deletion is pending.
  Polling now uses the error-code overload and requires confirmed absence;
  an access error never counts as successful reclamation.
  [ci-fix-debug.log](ci-fix-debug.log) passes all six snapshot tests and
  [reclamation-repeat-debug.log](reclamation-repeat-debug.log) passes that
  reclamation test 100 consecutive times.
- [ci-tidy-before.log](ci-tidy-before.log) records the Linux clang-tidy request
  for `[[nodiscard]]` on OldestSnapshot; [ci-fix-tidy.log](ci-fix-tidy.log)
  records the local check after adding the annotation. Final CI checks the
  repository configuration with clang-tidy 18.

## Full milestone validation

All final local gates and the recorded CI run use the clean implementation
commit `ca581d60c1f1563cbeed5576c8b7c1253799bc9a`. [windows.json](windows.json) records the clean status,
compiler and host. The evidence commit adds logs and this packet only.
The full 10,000-seed DB differential test remains enabled in every full suite.

| Gate | Result | Full output |
| --- | --- | --- |
| Native Windows MSVC Debug | PASS: 105 GoogleTests, all 5 CTest entries | [windows-debug.log](windows-debug.log) |
| Native Windows MSVC ASan | PASS: 105 GoogleTests, all 5 CTest entries | [windows-asan.log](windows-asan.log) |
| CI Windows MSVC Debug | PASS | [Windows-debug.log](ci-39469933974/Windows-debug.log) |
| CI Windows MSVC ASan | PASS | [Windows-asan.log](ci-39469933974/Windows-asan.log) |
| CI Ubuntu 24.04 Debug | PASS | [Linux-debug.log](ci-39469933974/Linux-debug.log) |
| CI Ubuntu 24.04 ASan + UBSan | PASS | [Linux-asan.log](ci-39469933974/Linux-asan.log) |
| CI Ubuntu 24.04 TSan | PASS: full suite, including concurrent readers during compaction | [Linux-tsan.log](ci-39469933974/Linux-tsan.log) |
| CI format, Ruff, hash oracle, clang-tidy 18 | PASS | [lint.log](ci-39469933974/lint.log) |

[CI run](https://github.com/ArnavLifelessCoder/stratadb/actions/runs/39469933974)
and [job metadata](ci-39469933974/summary.json) identify all six successful jobs.
MSVC provides ASan locally; Linux CI supplies UBSan and TSan.

Native `proc.kill()` smoke passed 12 cycles per preset:
[Debug JSON](crash-Windows-debug.json), [Debug output](crash-Windows-debug.log),
[ASan JSON](crash-Windows-asan.json), [ASan output](crash-Windows-asan.log).
Each of the four CI platform/preset test jobs also passed its 12-cycle smoke;
their platform-labeled JSON files are alongside the full logs in `ci-39469933974/`.
Every cycle recovered with SSTables present. This is supporting M5 evidence;
the 500-cycle M6 campaign has not started.

### Clean-tree Bloom measurement

[bloom-Windows-debug.json](bloom-Windows-debug.json) records five seeded trials
at 10 bits/key: 8,125 positives from
1,000,000 absent-key queries, an aggregate
0.8125% false-positive rate.
The theoretical rate recorded in each trial is
0.819372%.
Each trial is within twice that theoretical rate.
[bloom-measurement.log](bloom-measurement.log) contains the full script output.
This is an accuracy measurement, not a throughput benchmark.

## Acceptance evidence

| Criterion | Evidence |
| --- | --- |
| Met: random snapshots match historical reference maps | Random put/delete/atomic-batch stream with multiple live snapshots, point reads, full forward/reverse scans and bounded ranges during compaction and after reopen |
| Met: Seek, Next, Prev and range scans match reference | Iterator unit cases plus map-based DB comparisons; Prev is implemented |
| Met: concurrent readers during compaction are TSan clean | Full CI TSan PASS on ca581d6 |
| Met: no file leaks after old views release | Hold a real obsolete SSTable through both a Snapshot and iterator, release them in order, wait for reclamation, then compare the complete directory with the manifest's live file set |
| Met: tombstones do not resurrect deeper values | Unit retention tests and native L1-to-L2 compaction with a deletion over an older L2 value, checked before and after reopening |
| Met: durability protocol retained | CRC-checked input reads, synced output files and atomic publication, then a synced manifest edit before inputs become obsolete; failed publication keeps inputs recoverable |

## Limits and next gate

Compaction currently loads its input streams and merged entries into memory.
Iterators materialize visible rows and pin their captured Version. Memory usage
can therefore grow with scan/compaction input and with long-lived snapshots.
The 2 MiB output target is not a hard cap for one large value or key history.
Round-robin pointers reset on Open; the append-only manifest is not rotated.

Cleanup failure may leave an unreferenced file for a later retry or Open.
The no-leak tests cover successful reclamation after every owning view is released.
No benchmark throughput or LevelDB comparison is claimed. The 12-cycle
flush/compaction smoke remains separate from the M6 crash campaign.

Wait for owner approval before merging M5 into main or starting M6.
