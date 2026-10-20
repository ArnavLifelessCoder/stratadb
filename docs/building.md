# Building and testing

M5 adds compaction, snapshots and stable iterators over the WAL, memtables and
SSTables. See [the storage design](memtable-wal.md), [the SSTable format](sstable.md),
[flush recovery](flush-recovery.md), and [compaction and snapshots](compaction-snapshots.md)
for contracts and current limits.

## Windows

Install Visual Studio 2022 Build Tools with the **Desktop development with C++**
workload, including the Windows SDK, C++ CMake tools and C++ AddressSanitizer.
The scripts use the x64 MSVC compiler and Ninja.

From PowerShell in the repository:

```powershell
.\scripts\build.ps1 debug
.\scripts\build.ps1 asan
```

For development, use `.\scripts\build.ps1 debug -Quick` (or pass `--quick`
directly to `lsm_tests` on either platform). Quick mode runs every test but
reduces the DB differential workload to 100 seeds. It prints its mode and is
not a milestone gate. Default CTest, CI and `verify.ps1` keep all 10,000 seeds.
`-Quick` can also be combined with `-TestFilter`.

Run `.\scripts\verify.ps1` to record both complete build/test logs and Windows
toolchain metadata in `results/m5/`. Pass `-Milestone NAME` to select another folder.
Use `-CleanEvidence` to require an unchanged committed tree and write to the ignored
`build/verification-m5/` folder. Copy finished logs to results only after all
clean-tree measurements are complete.

Alternatively, in an x64 VS 2022 developer shell:

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --verbose
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --verbose
```

MSVC Debug uses `/Od /Zi /RTC1`; ASan uses `/O1 /Zi /fsanitize=address`
and disables incremental linking. MSVC has no UBSan or TSan. Linux runs
ASan plus UBSan, and has a separate TSan job. Sanitizers are separate builds.
Project sources and tests use `/W4 /WX /permissive-`.

## Linux

Use CMake 3.20+, Ninja, Git, and GCC 12+ or Clang 15+:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --verbose
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --verbose
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --verbose
```

Project sources and tests use `-Wall -Wextra -Wpedantic -Werror`. Sanitized
builds use `-O1 -g`; Debug uses `-O0 -g`. The release preset is reserved for
benchmarks: GCC/Clang use `-O3 -march=native -DNDEBUG`; MSVC uses `/O2 /DNDEBUG`.
GoogleTest and the optional Google Benchmark dependency are pinned to commits
and fetched inside the build directory. No LevelDB dependency is configured.

## Reproducible tests and formatting

The test executable accepts `--seed=N` (an unsigned decimal 64-bit integer),
prints the seed, and accepts the usual GoogleTest filter flags. For example,
from a developer shell on Windows:

```powershell
.\build\debug\lsm_tests.exe --seed=42
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements-dev.txt
python scripts/check_format.py
ruff check .
ruff format --check .
```

On Linux, use `build/debug/lsm_tests` and activate with
`. .venv/bin/activate`. Formatting checks only visit project source directories,
so fetched dependencies are excluded.

CI builds and tests Debug and ASan on Windows and Ubuntu 24.04, runs TSan on
Ubuntu 24.04, and uploads complete test logs and platform-labeled crash results.
The concurrency tests include multi-key skiplist insertion with older sequence
reads, four readers alongside four DB writers, and concurrent mapped table reads.

Windows CI uses MSVC `cl` from the installed Visual Studio developer environment.
The `windows-latest` image can move to a newer Visual Studio generation, so CI
does not pin `vsversion` or override `CC`/`CXX`. Local `build.ps1` continues to use
VS 2022 MSVC. The shared `scripts/msvc_environment.ps1` discovers the installed
toolchain with `vswhere` and initializes its x64 developer environment.

Run clang-tidy against a configured compilation database:

```sh
python scripts/check_tidy.py --build build/debug --clang-tidy clang-tidy-18
```

CI runs this command with the repository's `.clang-tidy`, treating findings as
errors. On Windows, use the x64 clang-tidy executable under
`VC/Tools/Llvm/x64/bin` from an initialized VS developer shell.

Run the process-kill smoke test after building:

```powershell
python tests/crash_smoke.py --executable build/debug/lsm_db_process.exe --output build/crash-Windows-debug.json
```

On Linux omit `.exe` and use a Linux-labeled output filename. The script uses
only the Python standard library. It keeps temporary databases inside the
build directory and records platform, seed and outcomes in JSON. Its one-byte
memtable threshold forces flushes; the JSON records SSTable counts after recovery.

Record five seeded bloom false-positive accuracy trials:

```powershell
python scripts/measure_bloom.py --binary build/debug/lsm_tests.exe --output build/bloom-Windows-debug.json --compiler "MSVC 19.44.35229.0" --flags "/Od /Zi /RTC1 /W4 /WX"
```

Start from a clean tree, keep redirected logs under build, and copy the completed
JSON into the current milestone's results folder afterwards. Use the actual
compiler and flags of your build.
On Linux omit `.exe` and label
the result accordingly. The test checks all inserted keys and queries a disjoint
absent-key set. It verifies the measured rate is within a factor of two of the
theoretical rate for seven probes and ten bits per key.
