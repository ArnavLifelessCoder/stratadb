# Building and testing

M1 supplies the utility library and tests. Database operations begin in M2.

## Windows

Install Visual Studio 2022 Build Tools with the **Desktop development with C++**
workload, including the Windows SDK, C++ CMake tools and C++ AddressSanitizer.
The scripts use the x64 MSVC compiler and Ninja.

From PowerShell in the repository:

```powershell
.\scripts\build.ps1 debug
.\scripts\build.ps1 asan
```

Run `.\scripts\verify.ps1` to record both complete build/test logs and Windows
toolchain metadata in `results/m1/`.

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

CI builds and tests Debug and ASan on both Windows and Linux, runs TSan on
Linux, and uploads complete test logs. M1 has no project concurrency; the TSan
job is in place for subsequent milestones. CI can only run after the owner
approves a push.
