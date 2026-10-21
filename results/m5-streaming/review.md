# M5 streaming iterator revision

Source commit: `6eb4e14222d369dc365ff57bda342d99dcb1d666` on `m5`. This supersedes the materialized-iterator
submission in [the original packet](../m5/review.md). Main remains the approved
M4 commit `49f92f18995115bc40b0261f40d36dcc1ef6fd1d`; M6 has not started.

## Built

- BlockIterator uses restart-point binary search for Seek and supports both
  traversal directions without decoding a full block into entries.
- TableIterator retains its reader, navigates the table index, and validates CRC
  every time it loads a data block. Memtable cursors retain their skiplist and
  acquire-load links while writers publish new nodes.
- MergingIterator owns child iterators and a heap of child indices. DBIter
  filters by the captured sequence, removes obsolete versions and tombstones,
  detects conflicting duplicate internal keys, and supports direction changes.
- NewIterator captures references and sequence under the DB mutex. It copies no
  entries and reads no data blocks at creation. L0 uses individual cursors;
  each disjoint higher level uses one lazy cursor, so Seek avoids unrelated tables.
- Iterators retain their Version and both memtables independently of snapshots.
  Reclamation waits for pinned readers and resumes after iterator release.
- WAL append/sync now holds only the writer mutex; publication uses the read-view
  mutex, and atomic counters make concurrent stats reads safe.
- Compaction feeds CompactEntries from TableIterators. It buffers one output
  chunk, persists file-number reservations, and holds the manifest mutex only
  for reservation/commit. Table writes retain file sync and atomic rename.
- Full snapshot differential coverage is now 100 seeds; quick mode uses two.
  The original full 10,000-seed DB differential gate is unchanged.

The public header `include/lsm/db.hpp` is unchanged by this revision.

## Required checks

| Requirement | Evidence |
| --- | --- |
| Streaming block/table/memtable iterators | Navigation and randomized seek tests compare with full-decoder/exported references; unread-block corruption is detected on transition. |
| Merge children, filter snapshots in both directions | 10,000 mixed merge navigation operations, tie-order direction changes, and 2,000 DB navigation operations at each of five read sequences match references. |
| No eager copying or table reads at creation | NewIterator only captures owning references; the short-scan regression fails on the old implementation with 750 data-block reads at creation. |
| Short scans avoid unrelated tables | Final test output: `ITERATOR_BLOCK_READS creation=0 short_scan=1 l1_tables=30`. The assertion permits the L0 fan-in plus two blocks. |
| Slow scans during compaction/reclamation | A slow iterator traverses old rows in both directions while a writer triggers successive compactions. Unpinned intermediate inputs disappear, pinned original inputs remain readable, and original files disappear after iterator release. Runs in Debug, ASan and TSan. |
| Historical snapshots match the map | 100 seeded streams take/release random snapshots, check point reads and forward/reverse/range scans during compaction, then reopen. |
| No file leaks after releasing versions | Directory contents are compared with the live manifest file set after release and worker shutdown. Startup orphan cleanup tests remain enabled. |
| Concurrent readers during compaction | Snapshot readers, atomic batches, concurrent stats collection and slow iteration run in the complete sanitizer suite. |

## Validation

Native Windows MSVC 19.44.35229.0 Debug and ASan each passed **116 tests**
plus all five CTest entries, with no sanitizer findings. Both runs used full mode:
10,000 DB seeds and 100 snapshot seeds. Debug CTest took 585.43 seconds; ASan
CTest took 1252.80 seconds. These are test-run durations, not benchmark results.

[CI run 39554902085](https://github.com/ArnavLifelessCoder/stratadb/actions/runs/39554902085)
passed all six jobs on the source commit above: Windows Debug/ASan, Linux
Debug/ASan+UBSan, Linux TSan, and lint. The Linux builds retain strict warnings
as errors. Each of the five test jobs ran the full suite, including the slow
reclamation test and the 100-seed snapshot differential test. Lint includes
the repository clang-tidy configuration. No test seed reductions or sanitizer
exclusions were used.

Full output is retained, not just summaries:

- [Windows Debug](windows-debug.log)
- [Windows ASan](windows-asan.log)
- [Windows machine and source metadata](windows.json)
- [CI Windows Debug](ci-Windows-debug.log)
- [CI Windows ASan](ci-Windows-asan.log)
- [CI Linux Debug](ci-Linux-debug.log)
- [CI Linux ASan/UBSan](ci-Linux-asan.log)
- [CI Linux TSan](ci-Linux-tsan.log)
- [CI lint](ci-lint.log)
- [CI job metadata](ci.json)
- [Local clang-tidy](clang-tidy.log) and [format/Ruff checks](format.log)

The native command is
`scripts/verify.ps1 -Milestone m5-streaming -CleanEvidence`. Both presets run
verbose CTest in full mode. Verification requires a clean committed tree at
start and an unchanged tree and HEAD at finish. Measurement and process-kill
outputs were written under ignored build paths before being copied here.

Both native presets passed 12 process-kill smoke cycles each:
[Debug](crash-Windows-debug.json), [ASan](crash-Windows-asan.json).
The four CI platform/preset combinations also passed 12 cycles each:
[Windows Debug](ci-crash-Windows-debug.json),
[Windows ASan](ci-crash-Windows-asan.json),
[Linux Debug](ci-crash-Linux-debug.json),
[Linux ASan](ci-crash-Linux-asan.json).
Every cycle produced SSTables and passed recovery verification; JSON labels its
actual platform and retains the acknowledged count and kill return code.

[Five clean-tree bloom accuracy trials](bloom-Windows-debug.json) ran one million
absent-key queries, with 8,125 positives (aggregate rate 0.8125%).
[Full measurement output](bloom-Windows-debug.log) and
[supplementary machine metadata](machine.json) are included.
This is filter accuracy evidence, not a throughput benchmark.

The standalone native ASan crash process initially exited before startup with
Windows missing-DLL status `0xC0000135`. Initializing
`scripts/msvc_environment.ps1 -Version '[17.0,18.0)'` before the Python harness
resolved the runtime lookup; the same binary then passed all cycles.

## Regression and component evidence

[eager-scan-before.log](eager-scan-before.log) records the required failing test
against MaterializedIterator. [tombstone-before.log](tombstone-before.log)
records the failing invalid-tombstone cursor test before its fix. Component
logs cover block, table, memtable, merge, DB iteration, lifetime, compaction
and WAL locking tests after each implementation step.

The initial local MSVC link returned LNK1163; rebuilding the same target without
a source change succeeded. Both outputs are retained in
[link-before.log](link-before.log) and [link-retry.log](link-retry.log).
Clang-tidy diagnostics led to explicit initializers, reserved child capacity
and simpler duplicate tracking; final lint output is clean.

## Files changed

Relative to rejected M5 commit `cda0032edb9d2221e8de09d8ea8e62c416b54ead`:

| Area | Files |
| --- | --- |
| Build | `CMakeLists.txt` |
| Iterator foundation | `src/internal_iterator.hpp`, `src/merging_iterator.hpp`, `src/merging_iterator.cpp` |
| Block/table iteration | `src/table/block_iterator.hpp`, `src/table/block_iterator.cpp`, `src/table/table_iterator.hpp`, `src/table/table_iterator.cpp`, `src/table/table.hpp`, `src/table/table.cpp` |
| Memtable and DB views | `src/memtable.hpp`, `src/memtable.cpp`, `src/iterator.hpp`, `src/iterator.cpp`, `src/db.cpp` |
| Compaction | `src/compaction.hpp`, `src/compaction.cpp` |
| WAL counters | `src/wal.hpp`, `src/wal.cpp` |
| Tests and reference cursor | `tests/block_iterator_test.cpp`, `tests/merging_iterator_test.cpp`, `tests/table_test.cpp`, `tests/memtable_test.cpp`, `tests/iterator_test.cpp`, `tests/compaction_test.cpp`, `tests/snapshot_test.cpp`, `tests/vector_iterator.hpp` |
| Documentation/evidence | `docs/building.md`, `docs/compaction-snapshots.md`, historical notice in `results/m5/review.md`, and this `results/m5-streaming/` directory |

## Limits and review boundary

Iterators allocate cursor/heap state and copy current keys/values as they advance.
They retain table metadata, bloom filters, mappings and pinned memtables through
their readers and Version. Compaction buffers one output chunk targeting 2 MiB;
one large value or one key's history can exceed that target. These are not
throughput or bounded-total-memory benchmark claims.

All snapshots/iterators must be released before DB destruction, as previously
approved. The 500-cycle durability campaign remains M6. No M6 work or main merge
is included. Awaiting owner approval of M5.
