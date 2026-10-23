# M8 verification

Built from `git archive a71e4e6` on the development machine (Windows 11, MSVC, see
each JSON's `platform`) by building the exported copy, running the full CTest suites
under the `debug` and `asan` presets, and then `python/crash_test.py` campaigns.

| File | Result |
|---|---|
| `test-debug.log` | GoogleTest: 125 tests passed; CTest: 5/5 entries passed |
| `test-asan.log` | GoogleTest: 125 tests passed; CTest: 5/5 entries passed |
| `crash-Windows-asan-async.json` | 50/50 cycles, 7158 acknowledged batches, 0 counted as lost, deepest level L1 |
| `crash-Windows-asan-sync.json` | 50/50 cycles, 6981 acknowledged batches, 0 counted as lost, deepest level L1 |
| `crash-Windows-debug-async.json` | 200/200 cycles, 41874 acknowledged batches, 4 counted as lost, deepest level L2 |
| `crash-Windows-debug-sync.json` | 200/200 cycles, 42313 acknowledged batches, 0 counted as lost, deepest level L2 |

Sync campaigns may not lose any acknowledged batch, and none did. Async campaigns
allow a lost tail. These async runs predate commit 4e4aa59, which made the harness
prefer the fully acknowledged prefix; before it, a final batch that changed nothing
(a delete of an absent key) was counted as lost. Their loss counts are therefore
upper bounds. Pass/fail is unaffected.

## Async rerun with the fixed harness

Rerun from `git archive e72c857` (the m8 tip, which includes the harness fix
from 4e4aa59 and all M6-M8 engine code):

| File | Result |
|---|---|
| `crash-Windows-debug-async-rerun.json` | 500/500 cycles, 103260 acknowledged batches, 0 lost, deepest level L2 |
| `crash-Windows-asan-async-rerun.json` | 100/100 cycles, 13832 acknowledged batches, 0 lost, deepest level L1 |

With the acknowledged prefix tried first, no acknowledged async batch was lost
in 600 kill cycles, as expected for process kills that leave the OS cache intact.

## Final engine commit

Rerun from `git archive 2d48b85`, the m8 commit that adds score-based level
picking and mid-compaction flushes (both change when background work runs).
Built with zero compiler warnings in both presets.

| File | Result |
|---|---|
| `test-debug.log` | GoogleTest 126 tests passed; CTest 5/5 entries passed; `GROUP_COMMIT writes=1600 wal_records=164 wal_syncs=164` |
| `test-asan.log` | GoogleTest 126 tests passed; CTest 5/5 entries passed; `GROUP_COMMIT writes=1600 wal_records=167 wal_syncs=167` |
| `crash-Windows-asan-async.json` | 50/50 cycles, 10784 acknowledged batches, 0 lost, deepest level L2 |
| `crash-Windows-asan-sync.json` | 50/50 cycles, 9912 acknowledged batches, 0 lost, deepest level L2 |
| `crash-Windows-debug-async.json` | 200/200 cycles, 65338 acknowledged batches, 0 lost, deepest level L2 |
| `crash-Windows-debug-sync.json` | 200/200 cycles, 67378 acknowledged batches, 0 lost, deepest level L2 |
