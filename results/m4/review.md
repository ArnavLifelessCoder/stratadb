# M4 review packet

Implementation commit: `e4c689ff02496d20ab940390343c569ad11de638` on `m4`.
M3 was fast-forwarded into `main` at `4285083910226007700d45fe99cd597d7f69fa13`
and pushed after owner authorization. M4 is on branch `m4`; M5 has not started.

## Built

- Memtable rotation to one immutable table, a background flush worker, L0 table
  publication, and reads spanning the active table, immutable table and L0 files.
- CRC-protected manifest edits, shared immutable versions, numbered WALs and
  tables, and atomic `CURRENT` publication. Recovery replays only the referenced
  WALs and rebuilds an interrupted pending flush.
- File sync before atomic rename, manifest sync before publishing versions,
  old-WAL retention until the table edit is durable, and sticky write failures.
- Validation of active Options before filesystem mutation, legacy WAL migration,
  and accounting for metadata and table writes.
- Tests for map equivalence across repeated flushes/reopens, tombstones, large
  values, interrupted publication, missing files, injected failures and concurrent
  readers/writers.

The public headers under `include/lsm/` remain unchanged. The protocol and its
failure behavior are documented in [flush-recovery.md](../../docs/flush-recovery.md).

## M3 review follow-ups

| Request | Resolution |
| --- | --- |
| Avoid eager data-block checks at Open | Open retains footer, index, filter and layout checks. Data CRCs run on reads; explicit `Verify()` performs a complete scan. |
| Corrupt a block after Open | Native mapped-file regression changes a data byte through another handle, syncs it, then requires Get and Verify to return Corruption. |
| Binary-search restart points | Get uses `SeekBlock`: binary-search full restart keys, then decode one restart group and possibly its successor. Differential tests compare against full decoding and bound decoded entries. |
| Windows obsolete-table deletion | Recorded for M5 in [follow-ups.md](../../docs/follow-ups.md). |
| Clean-tree measurement JSON | Measurement runs write under ignored build paths; completed output is copied into results afterward. Source commit and empty Git status are recorded. |

## Files changed

[files.txt](files.txt) records the complete inventory relative to accepted M3.

| Area | Files |
| --- | --- |
| Flush and recovery | src/db.cpp, src/version_set.hpp, src/version_set.cpp |
| Frozen memtable and numbered WAL support | src/memtable.hpp, src/memtable.cpp, src/wal.hpp, src/wal.cpp |
| Lazy validation and restart seeking | src/table/table.hpp, src/table/table.cpp, src/table/block.hpp, src/table/block.cpp |
| Windows mapped-file sharing | src/platform/mapping_windows.cpp |
| Tests | tests/flush_test.cpp, version_set_test.cpp, block_test.cpp, table_test.cpp, memtable_test.cpp, wal_test.cpp, db_test.cpp |
| Crash and validation harnesses | tests/db_process.cpp, tests/crash_smoke.py, scripts/verify.ps1, CMakeLists.txt |
| Documentation | docs/flush-recovery.md, sstable.md, memtable-wal.md, building.md, follow-ups.md |
| Evidence | results/m4/ |

## Validation

The full local run and measurement use the implementation commit above with a
clean tree. [windows.json](windows.json) records the machine, toolchain, commit
and source status. Local compiler: VS 2022 MSVC 19.44.35229.0.

Debug passed all 84 GoogleTests and all five CTest entries. ASan passed all
84 GoogleTests (648.034 seconds for that test executable). Four of five CTest
entries passed in the initial run; `seed_reject_overflow` printed its expected
error but exceeded its 10-second process-exit timeout. The unchanged check
passed in 0.25 seconds on the immediate rerun. Both complete outputs are retained;
the metadata reports `passed_after_retry`. No sanitizer finding was reported.

| Check | Complete evidence |
| --- | --- |
| Native Windows Debug | [windows-debug.log](windows-debug.log) |
| Native Windows MSVC ASan | [windows-asan.log](windows-asan.log), [exit-timeout retry](windows-asan-retry.log) |
| Windows Debug forced-flush process kills | [log](crash-Windows-debug.log), [JSON](crash-Windows-debug.json) |
| Windows ASan forced-flush process kills | [log](crash-Windows-asan.log), [JSON](crash-Windows-asan.json) |
| Local x64 clang-tidy with repository checks | [tidy-Windows.log](tidy-Windows.log) |
| Formatting, Ruff and hash oracle | [lint-Windows.log](lint-Windows.log) |

Both local crash-smoke runs passed 12 cycles each (six sync and six async),
with at least one recovered SSTable in every cycle.

CI run:
[39452107094](https://github.com/ArnavLifelessCoder/stratadb/actions/runs/39452107094).
Full job metadata: [summary.json](ci-39452107094/summary.json).
All six jobs passed on the implementation commit. Every test job passed
84 GoogleTests and all five CTest entries, including Linux TSan. No ASan,
UBSan or TSan finding was reported.

| Job | Full output |
| --- | --- |
| Windows Debug | [118729330618.log](ci-39452107094/118729330618.log) |
| Windows ASan | [118729329406.log](ci-39452107094/118729329406.log) |
| Ubuntu 24.04 Debug | [118729329552.log](ci-39452107094/118729329552.log) |
| Ubuntu 24.04 ASan + UBSan | [118729329464.log](ci-39452107094/118729329464.log) |
| Ubuntu 24.04 TSan | [118729329018.log](ci-39452107094/118729329018.log) |
| Formatting, Ruff, hash oracle and clang-tidy | [118729329946.log](ci-39452107094/118729329946.log) |

Each CI Debug/ASan job also runs 6 sync and 6 async forced-flush kill cycles.
Platform-labeled results:
[Windows Debug](ci-39452107094/crash-Windows-debug.json),
[Windows ASan](ci-39452107094/crash-Windows-asan.json),
[Linux Debug](ci-39452107094/crash-Linux-debug.json),
[Linux ASan](ci-39452107094/crash-Linux-asan.json).

## Measured Bloom accuracy from a clean tree

[bloom-Windows-debug.json](bloom-Windows-debug.json) records source commit
`e4c689ff02496d20ab940390343c569ad11de638` and an empty `source_status`.
Output was first written under ignored `build/`, then copied into `results/m4/`
after completion. [bloom-measurement.log](bloom-measurement.log) contains the full
script output.

Five seeds, 10,000 inserted keys per seed and 200,000 disjoint absent-key queries
per seed produced 8,125 false positives out of 1,000,000 queries: **0.8125%**.
The theoretical value at ten bits/key and seven probes is about **0.819372%**.
All individual trials passed the factor-of-two check and checked every inserted
key for false negatives. These are accuracy measurements, not throughput results.

## Regression history

Failing tests were retained before the corresponding fixes:

- [lazy-reader-before.log](lazy-reader-before.log): Windows mapping initially
  prevented the native after-open corruption injection.
- [bootstrap-stats-before.log](bootstrap-stats-before.log): initial metadata
  writes were omitted from the disk-byte counter.
- [bootstrap-preservation-before.log](bootstrap-preservation-before.log):
  interrupted bootstrap could overwrite an existing initial WAL, and missing
  metadata with later files could initialize an empty database.

The final manifest/flush component run is in
[manifest-flush-final-debug.log](manifest-flush-final-debug.log).
The clean-evidence script's rejection of a dirty tree is recorded in
[clean-evidence-rejection.log](clean-evidence-rejection.log).

The first Linux CI build rejected an unbraced `if` around a GoogleTest macro
under `-Werror=dangling-else`; [gcc-before.log](gcc-before.log) preserves the
failure. Explicit braces fixed it, and the focused test passed in
[ci-fix-debug.log](ci-fix-debug.log).

The first local ASan run exceeded the old 600-second suite timeout during the
10,000-seed replay test; [windows-asan-timeout-before.log](windows-asan-timeout-before.log)
retains that run. The suite limit is now 1,800 seconds. No test, seed, sync,
rename or CRC check was removed.

## Acceptance evidence

| Criterion | Evidence |
| --- | --- |
| Reopen after any number of flushes matches the reference map | Deterministic 0, 1, 2, 9 and 32 forced-flush cases; 2,000 random operations with repeated reopens across both SSTables and the current WAL; empty keys/values, 1 MiB values, deletes and repeated versions |
| Recovery after interrupted flush publication | Tests reconstruct both a committed rotation and a published-but-uncommitted SSTable; recovery rebuilds the immutable table and accepts later writes |
| Safe failures | Injected manifest read/append/sync/truncate errors, missing live WAL/table checks, and a background table-publication failure all return errors while preserving recoverable state |
| Concurrent readers during flush | Four readers and two writers exercise publication and shared version lifetime; Linux TSan passed |
| Durability operations | Old WAL sync before rotation; new WAL and SSTable temp-file sync before atomic rename; manifest sync before publication; no old-WAL unlink until its table is durably referenced |

The M4 acceptance criterion and requested reader follow-ups are met. Local Debug
and ASan validation passed with the documented ASan CLI retry; all six CI jobs
passed, including the complete TSan suite.

## Limits and next gate

L0 files accumulate until M5 compaction. Recovery ignores unreferenced files;
a crash after the durable flush edit but before old-WAL removal can leave an
obsolete WAL. M5's no-file-leaks work must reclaim these files and test deletion
while Windows mappings remain open. The original legacy WAL is retained after
migration as an unused backup.

The append-only manifest is not rotated yet. A conservative scan for a later
valid frame can classify an embedded frame in a torn payload as Corruption;
this is documented and preserves the file for inspection. An error returned
after a write or publication operation does not prove the operation was absent
from disk; writes stop and recovery determines the durable state.

The process-kill checks are M4 smoke coverage, not the 500-cycle M6 campaign.
No throughput benchmark or LevelDB build was performed.

Wait for owner approval before merging M4 into main or starting M5.
