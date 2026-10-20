# M3 review packet

Implementation commit: `3aca8a787caf6f8812013d7bef5b158f21b459f1` on `m3`.
M2 was fast-forwarded into `main` at `8a734b0` and pushed after owner authorization.
M4 has not started.

## Built

- Prefix-compressed, CRC32C-protected data blocks with full-key restarts every
  16 entries, strict bounds checks and descending internal sequence ordering.
- Per-table double-hash bloom filters over distinct user keys, including
  tombstones. Ten bits per key uses seven probes; zero disables filtering.
- Streaming table builder, block index, fixed versioned footer and immutable
  publication through temp write, file sync, close and atomic rename.
- Read-only Windows and POSIX mappings; readers check all block CRCs and structure
  at Open, then support sequence-aware lookups and ordered entry reads.
- Native round-trip, fault-injection, corruption, concurrent-read and accuracy tests.

The public headers under `include/lsm/` are unchanged. DB table discovery, flushes,
manifests and Options limits remain M4 work. The format and tradeoffs are in
[the SSTable design](../../docs/sstable.md).

## M2 review follow-ups

| Item | Resolution |
| --- | --- |
| CreateDirectories durability comment | Corrected: POSIX syncs parent entries; Windows relies on filesystem metadata recovery and does not explicitly flush directories. |
| All-zero WAL tail | Added a failing regression, then repair through truncate and sync. Zeros followed by another record remain corruption. |
| Embedded valid WAL record in a torn value | Documented the conservative bytewise scan's possible false Corruption result and why it preserves the file. |
| Multi-key concurrent memtable reads | Added four readers over 64 keys, inserts in permuted order, tombstones, and older sequence snapshots; passed Linux TSan. |
| DB Options validation | Deferred to M4 when limits such as memtable_bytes become active, as requested. Standalone table settings are validated now. |

The original WAL failure is preserved in [wal-zero-before.log](wal-zero-before.log).
The permanent test checks 1, 9, 512 and 4096 zero bytes, both at EOF and before
another valid record.

## Files changed

[files.txt](files.txt) contains the complete inventory relative to accepted M2.

| Area | Files |
| --- | --- |
| Block format | src/table/format.hpp, block.hpp, block.cpp |
| Bloom | src/table/bloom.hpp, bloom.cpp |
| Table builder/reader | src/table/table.hpp, table.cpp |
| Mapping platform layer | src/platform/mapping.hpp, mapping_windows.cpp, mapping_posix.cpp |
| Recovery follow-ups | src/wal.cpp, src/platform/file.hpp, docs/memtable-wal.md |
| Tests | tests/block_test.cpp, bloom_test.cpp, mapping_test.cpp, table_test.cpp, wal_test.cpp, memtable_test.cpp |
| Build and measurement | CMakeLists.txt, scripts/measure_bloom.py, scripts/verify.ps1 |
| Documentation and evidence | docs/sstable.md, docs/building.md, results/m3/ |

## Full local test output

Machine and toolchain metadata are in [windows.json](windows.json).
Native Windows 11, AMD Ryzen 9 8940HX, VS 2022 MSVC 19.44.35229.0.

| Check | Result | Complete output |
| --- | --- | --- |
| Debug | 63 GoogleTests, 5/5 CTest entries passed | [windows-debug.log](windows-debug.log) |
| MSVC ASan | 63 GoogleTests, 5/5 CTest entries passed | [windows-asan.log](windows-asan.log) |
| Debug process-kill smoke | 6 sync + 6 async cycles passed | [log](crash-Windows-debug.log), [JSON](crash-Windows-debug.json) |
| ASan process-kill smoke | 6 sync + 6 async cycles passed | [log](crash-Windows-asan.log), [JSON](crash-Windows-asan.json) |
| x64 clang-tidy | Passed with repository checks and findings treated as errors | [tidy-Windows.log](tidy-Windows.log) |
| Formatting, Ruff, independent hash oracle | Passed | [lint-Windows.log](lint-Windows.log) |

Component test logs are retained separately: [review follow-ups](review-followups-debug.log),
[blocks](block-debug.log), [bloom](bloom-debug.log), [mappings](mapping-debug.log),
[tables](table-debug.log), and [final component rerun](table-final-debug.log).
Clang-tidy initially flagged swappable helper arguments and modernization items;
the diagnostics are preserved in [tidy-Windows-before.log](tidy-Windows-before.log).
Those findings were fixed before the implementation commit.
The final tidy log contains suppressed SDK/STL diagnostics, with no unsuppressed
project findings.

## CI results

All six jobs passed on the implementation commit:
[run 39434307912](https://github.com/ArnavLifelessCoder/stratadb/actions/runs/39434307912).
The complete job metadata is in [summary.json](ci-39434307912/summary.json).

| Job | Result | Full job log |
| --- | --- | --- |
| Windows Debug, MSVC 19.51.36260.0 | Passed | [118667166619.log](ci-39434307912/118667166619.log) |
| Windows ASan, MSVC 19.51.36260.0 | Passed | [118667166407.log](ci-39434307912/118667166407.log) |
| Ubuntu 24.04 Debug, GCC 13.3.0 | Passed | [118667166609.log](ci-39434307912/118667166609.log) |
| Ubuntu 24.04 ASan + UBSan, GCC 13.3.0 | Passed | [118667165984.log](ci-39434307912/118667165984.log) |
| Ubuntu 24.04 TSan, GCC 13.3.0 | Passed | [118667166161.log](ci-39434307912/118667166161.log) |
| Formatting, Ruff, hash oracle, clang-tidy-18 | Passed | [118667165703.log](ci-39434307912/118667165703.log) |

All test jobs ran the full suite; each Debug/ASan job also passed 12 process-kill
smoke cycles. Extracted platform-labeled crash results:
[Windows Debug](ci-39434307912/crash-Windows-debug.json),
[Windows ASan](ci-39434307912/crash-Windows-asan.json),
[Linux Debug](ci-39434307912/crash-Linux-debug.json),
[Linux ASan](ci-39434307912/crash-Linux-asan.json).
These smoke checks do not replace the 500-cycle M6 campaign.

## Measured bloom accuracy

Command and toolchain reproduction are in [building.md](../../docs/building.md).
The script ran the real C++ filter with seeds 24301 through 24305, 10,000 inserted
keys per trial, and 200,000 disjoint absent-key queries per trial. Every inserted
key was checked for false negatives.

Across 1,000,000 absent-key queries there were 8,125 false positives: **0.8125%**.
The theoretical probability for seven probes and ten bits per key is
`(1 - exp(-7/10))^7`, approximately **0.819372%**. Every individual trial passed
the factor-of-two check. These are filter accuracy results, not throughput figures.

Raw counts and environment: [bloom-Windows-debug.json](bloom-Windows-debug.json).
Full script output: [bloom-measurement.log](bloom-measurement.log).

## Acceptance criteria

| Criterion | Evidence | Status |
| --- | --- | --- |
| Round trips across key/value mixes, empty and 1 MiB values | Four block targets, bloom on/off, binary and empty keys, tombstones and versions crossing block boundaries; block test also includes a 1 MiB key | Met |
| Measured bloom FPR within 2x theoretical at 10 bits/key | Five recorded trials, aggregate 0.8125%, no false negatives | Met |
| A flipped byte in any block detected by CRC | Every byte of a representative complete table flipped individually, including data, filter, index and footer; every truncation tested too | Met |
| Local Debug and ASan | Both passed: 63 GoogleTests and 5/5 CTest entries per preset | Met |
| Linux TSan for concurrent code | Full suite green, including multi-key memtable readers and mapped-table readers | Met |
| Durability operations retained | File sync before rename, native atomic rename protocol; every injected append/sync failure propagates and poisons the builder | Met |

## Limits and next gate

Open validates all blocks, so its cost is linear in table size. Lookups decode
one whole block; restart arrays are validated but suffix-only decoding and block
caching are not implemented. These choices and mapping lifetime constraints are
documented in the format note.

The caller must exclusively own each destination and temp sibling. M4 supplies
file numbering and manifest recovery, including uncertain publication after a
directory-sync failure. A destination is preserved after such an error.

Wait for owner approval before merging M3 or starting M4. M4 must validate active
DB Options, including rejected memtable_bytes values, with tests.
