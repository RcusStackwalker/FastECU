# Windows serial verification — 2026-10-01

The historical Windows-only serial-test crash was **not reproduced in this
campaign**. All nine Windows-compatible serial targets passed 50 fresh
executions with normal scheduling and another 50 on a separate runner with
sequential test scheduling: **900 target executions and 4,300 GoogleTest case
executions**, with no failed, missing, cached, skipped, or disabled executions.

This completes the one-off verification of the previously unverified crash
report after the GoogleTest migration. It does not establish a root cause,
prove that the migration fixed the crash, or rule out intermittent failures
under other environments or workloads. No test was quarantined or excluded
because of a failure, and no application or serial-test code was changed.

## Provenance

- [Campaign PR #457](https://github.com/RcusStackwalker/FastECU/pull/457).
- [Campaign run 36860368906, attempt 1](https://github.com/RcusStackwalker/FastECU/actions/runs/36860368906).
- [Normal scheduling job](https://github.com/RcusStackwalker/FastECU/actions/runs/36860368906/job/110362807868)
  and [sequential scheduling job](https://github.com/RcusStackwalker/FastECU/actions/runs/36860368906/job/110362808370).
- Campaign commit: `62e2eb66259853b0704ccf04084034bcbb2a39cc`.
- Actual checkout (PR merge commit): `062b52357fdebaf6a8d388f74dd4bdcac4f617ec`.
- Application baseline: `05172b8a`.

Both independent runners used the same environment:

| Component | Recorded value |
| --- | --- |
| Runner selector | `windows-latest` |
| Runner image | `win25-vs2026`, version `20260925.250.1` |
| Visual Studio | Enterprise 2026, installation `18.10.12217.157` |
| Qt | `6.8.3`, package `win64_msvc2022_64` |
| Bazel / Bazelisk | `9.1.1` / `v1.28.1` |
| Installed LLVM | `clang 20.1.8`, target `x86_64-pc-windows-msvc` |
| Build configuration | `--config=release` |

The environment setup matched the ordinary Windows PR job. The Qt package's
architecture label does not describe the installed Visual Studio version;
both are recorded above. LLVM is an installed tool, not a claim that Clang
compiled the tests.

## Results

All labels below are relative to `//src/platform/desktop/common/serial`.
Each row has zero failed or missing target executions in either job.

| Target suffix | Cases per execution | Normal passes | Sequential passes |
| --- | ---: | ---: | ---: |
| `:desktop_serial_factory_test` | 3 | 50 | 50 |
| `:j2534_driver_selection_windows_test` | 1 | 50 | 50 |
| `:serial_idle_test` | 1 | 50 | 50 |
| `/direct/common:test_direct_backend` | 6 | 50 | 50 |
| `/direct/windows:direct_backend_hooks_windows_test` | 3 | 50 | 50 |
| `/facade:test_facade_threading` | 22 | 50 | 50 |
| `/remote:test_remote_backend_smoke` | 1 | 50 | 50 |
| `/testing:fake_backend_test` | 3 | 50 | 50 |
| `/testing:test_fake_backed_serial` | 3 | 50 | 50 |
| **Total** | **43** | **450** | **450** |

The inventory was obtained with
`bazel query 'tests(//src/platform/desktop/common/serial/...)'`.
Only these three Unix-only targets were excluded from the expected Windows
inventory, according to their existing `target_compatible_with` declarations:

- `//src/platform/desktop/common/serial:j2534_driver_selection_unix_test`
- `//src/platform/desktop/common/serial/direct/unix:direct_backend_hooks_unix_test`
- `//src/platform/desktop/common/serial/direct/unix:test_direct_backend_pty`

No GoogleTest case was skipped or disabled in the 900 XML reports.

## Method and retained evidence

Each job invoked Bazel once, with a separate test process for each run:

```sh
bazel test -k --config=release --nocache_test_results \
  --runs_per_test=50 --flaky_test_attempts=1 \
  --noruns_per_test_detects_flakes --test_sharding_strategy=disabled \
  --build_event_json_file=<job-evidence-directory>/events.jsonl \
  //src/platform/desktop/common/serial/...
```

The sequential job additionally set `--local_test_jobs=1`. The normal job used
Bazel's default test concurrency. Tests retained their existing timeouts;
the test step had a 40-minute limit inside a 60-minute job limit. Neither
limit was reached.

Each artifact contains 450 per-attempt logs and 450 XML files, the raw Bazel
events and console log, actual command arguments, target/exclusion inventories,
environment/tool versions, exit code, and JSON/Markdown result summaries.
The downloaded evidence was independently checked for run numbers 1–50 for
every target, successful uncached BEP results, complete log/XML counts, and
zero XML failures, errors, disabled cases, or skips.

Artifacts `serial-campaign-normal-1` and `serial-campaign-sequential-1` have
30-day retention. This document preserves the conclusion, counts, scope,
environment, method, and provenance after those artifacts expire. The
[temporary workflow](https://github.com/RcusStackwalker/FastECU/blob/62e2eb66259853b0704ccf04084034bcbb2a39cc/.github/workflows/windows-serial-campaign.yml)
and its report checker remain available in the campaign commit; they were
removed from the final branch after evidence collection.

The ordinary PR checks also passed on the campaign commit, including the full
Windows/macOS/Linux Bazel jobs, SonarCloud, Gazelle, and the Android portable
core build. These checks supplement, but are not counted in, the totals above.

If the crash recurs, retain the failing binary, case output, environment, and
exit status and reopen focused investigation. Existing unbuffered serial-test
diagnostics remain in place. Do not attribute a recurrence to one suite
without evidence or discard unrelated coverage-test failures.
