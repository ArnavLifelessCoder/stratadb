# M1 CI review follow-up

M1 is not accepted yet. Every hosted job must pass on the repaired revision.

The owner pushed the local verification commit and then the seed-check correction.
The latest reviewed run is
[39419845404](https://github.com/ArnavLifelessCoder/stratadb/actions/runs/39419845404)
at `e9232a074367e931f295ddf0988967c0e5768e2d`.

| Job | Observed result | Complete output |
|---|---|---|
| Ubuntu Debug | Pass, 5/5 CTest entries | [log](../results/m1/ci-39419845404/118617764666.log) |
| Ubuntu ASan/UBSan | Pass, 5/5 CTest entries | [log](../results/m1/ci-39419845404/118617764755.log) |
| Ubuntu TSan | Pass, 5/5 CTest entries | [log](../results/m1/ci-39419845404/118617764417.log) |
| Lint | Pass | [log](../results/m1/ci-39419845404/118617764735.log) |
| Windows Debug | Failed before compilation: VS 2022 unavailable | [log](../results/m1/ci-39419845404/118617764716.log) |
| Windows ASan | Failed before compilation: VS 2022 unavailable | [log](../results/m1/ci-39419845404/118617764987.log) |

No project compiler warnings, UBSan findings or TSan findings appeared in the
successful Linux jobs. The logs include third-party action runtime deprecation
warnings; these are distinct from compiler and sanitizer diagnostics.

## Prepared fix

The prepared workflow leaves `vsversion` unpinned and uses MSVC `cl` from the
installed Visual Studio developer environment. The `CC`/`CXX=clang-cl` override
introduced in `1ad2646` was rejected in review and has been removed. Both required
runner labels remain `windows-latest` and `ubuntu-latest`, with six jobs in total.
Local MSVC builds are unchanged.

The observed Windows runner image is `windows-2025-vs2026`, which has VS 2026
installed. The earlier workflow failed while looking specifically for VS 2022.
The corrected workflow keeps the owner's compiler choice, MSVC `cl`.
The revised Windows build itself remains unverified until the commit is pushed.

The original bootstrap regression was written and run before the initial fix:

- [Failing regression output](../results/m1/ci-fix/regression-before.log)
- [Passing regression, lint and formatting output](../results/m1/ci-fix/regression-after.log)

Those two outputs describe the initial fix. The regression test now asserts only
that `vsversion` is unpinned; it does not require clang-cl.
See [the current validation output](../results/m1/ci-fix/msvc-config-validation.log).

Changed implementation/configuration files:

- `.github/workflows/ci.yml`: use the available MSVC environment without a compiler override,
  and run the regression test in lint.
- `tests/test_ci_configuration.py`: assert only that `vsversion` is unpinned.
- `requirements-dev.txt`: pin the YAML parser used by that test.

Full logs and machine-readable summaries are retained for both observed runs
under `results/m1/ci-39419669148/` and `results/m1/ci-39419845404/`.
The previous local MSVC Debug and ASan outputs remain linked from
[the original review packet](m1-review.md). The CI fix changes no C++ source.

The owner must push the prepared commits before the repaired workflow can be
verified. M1 stays open until all six jobs are green on the pushed revision.

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
These follow-ups are deferred while the M1 CI acceptance gate remains open.
