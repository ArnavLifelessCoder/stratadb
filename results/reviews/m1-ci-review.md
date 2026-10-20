# M1 CI review follow-up

All six hosted CI jobs pass on the repaired, pushed revision. The M1 CI blocker
is resolved. M2 has not started.

The owner authorized pushing the corrected workflow. The verified run is
[39421762788](https://github.com/ArnavLifelessCoder/stratadb/actions/runs/39421762788)
at `51bbd12774dcb33903955055c457dd5432c58859`.

| Job | Observed result | Complete output |
|---|---|---|
| Ubuntu Debug | Pass, 5/5 CTest entries | [log](../m1/ci-39421762788/118624198526.log) |
| Ubuntu ASan/UBSan | Pass, 5/5 CTest entries | [log](../m1/ci-39421762788/118624198492.log) |
| Ubuntu TSan | Pass, 5/5 CTest entries | [log](../m1/ci-39421762788/118624198012.log) |
| Lint | Pass, including the workflow regression | [log](../m1/ci-39421762788/118624198420.log) |
| Windows MSVC Debug | Pass, 5/5 CTest entries | [log](../m1/ci-39421762788/118624198386.log) |
| Windows MSVC ASan | Pass, 5/5 CTest entries | [log](../m1/ci-39421762788/118624198477.log) |

The logs identify GCC 13.3.0 on Linux and MSVC 19.51.36260.0 on Windows.
No project compiler warnings or sanitizer findings appeared in the successful jobs.
The logs include third-party action runtime deprecation warnings; these are
distinct from compiler and sanitizer diagnostics.

## Verified fix

The verified workflow leaves `vsversion` unpinned and uses MSVC `cl` from the
installed Visual Studio developer environment. The `CC`/`CXX=clang-cl` override
introduced in `1ad2646` was rejected in review and has been removed. Both required
runner labels remain `windows-latest` and `ubuntu-latest`, with six jobs in total.
Local MSVC builds are unchanged.

The observed Windows runner image is `windows-2025-vs2026`, which has VS 2026
installed. The earlier workflow failed while looking specifically for VS 2022.
The corrected workflow keeps the owner's compiler choice, MSVC `cl`.
Both Windows builds and their complete test suites now pass.

The original bootstrap regression was written and run before the initial fix:

- [Failing regression output](../m1/ci-fix/regression-before.log)
- [Passing regression, lint and formatting output](../m1/ci-fix/regression-after.log)

Those two outputs describe the initial fix. The regression test now asserts only
that `vsversion` is unpinned; it does not require clang-cl.
See [the current validation output](../m1/ci-fix/msvc-config-validation.log).

Changed implementation/configuration files:

- `.github/workflows/ci.yml`: use the available MSVC environment without a compiler override,
  and run the regression test in lint.
- `tests/test_ci_configuration.py`: assert only that `vsversion` is unpinned.
- `requirements-dev.txt`: pin the YAML parser used by that test.

Full logs and machine-readable summaries are retained for both observed runs
under `results/m1/ci-39419669148/` and `results/m1/ci-39419845404/`.
Those earlier runs document the bootstrap failure. The successful run is saved
under `results/m1/ci-39421762788/`, with a
[machine-readable summary](../m1/ci-39421762788/summary.json).
The previous local MSVC Debug and ASan outputs remain linked from
[the original review packet](m1-review.md). The CI fix changes no C++ source.

The local Debug and ASan suites and all six hosted jobs pass. CRC32C RFC vectors
and varint round-trip fuzz acceptance criteria remain satisfied.
See the [complete file inventory](../m1/final-files.txt) for the M1 files.
M2 remains gated on the owner's milestone approval.

## Deferred M2 requirements

After M1 acceptance:

- Create branch `m2` from the accepted `main`.
- Add an SSE4.2 CRC32C implementation with the table implementation as fallback.
  Test both against the RFC vectors and bitwise reference.
- Execute clang-tidy using the repository configuration in CI or a script.

These tasks have not been started, and the specification files have not been edited.

## Non-blocking cleanup

Before M9, move `docs/m1-review.md` and `docs/m1-ci-review.md` out of `docs/` and
repair their links. Also upgrade the actions still targeting Node 20.
These follow-ups remain scheduled for later milestones.
