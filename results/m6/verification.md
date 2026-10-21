# M6 verification

Built from `git archive 137ef43` on the development machine (Windows 11, MSVC, see
each JSON's `platform`) by building the exported copy, running the full CTest suites
under the `debug` and `asan` presets, and then `python/crash_test.py` campaigns.

| File | Result |
|---|---|
| `test-debug.log` | GoogleTest: 116 tests passed; CTest: 5/5 entries passed |
| `test-asan.log` | GoogleTest: 116 tests passed; CTest: 5/5 entries passed |
| `crash-Windows-asan-async.json` | 100/100 cycles, 12655 acknowledged batches, 2 counted as lost, deepest level L1 |
| `crash-Windows-asan-sync.json` | 100/100 cycles, 12177 acknowledged batches, 0 counted as lost, deepest level L1 |
| `crash-Windows-debug-async.json` | 500/500 cycles, 96664 acknowledged batches, 5 counted as lost, deepest level L2 |
| `crash-Windows-debug-sync.json` | 500/500 cycles, 93170 acknowledged batches, 0 counted as lost, deepest level L2 |

Sync campaigns may not lose any acknowledged batch, and none did. Async campaigns
allow a lost tail. These async runs predate commit 4e4aa59, which made the harness
prefer the fully acknowledged prefix; before it, a final batch that changed nothing
(a delete of an absent key) was counted as lost. Their loss counts are therefore
upper bounds. Pass/fail is unaffected.
