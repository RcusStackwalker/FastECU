# clang-tidy CI speed-up: profiling, scoped platform runs, parallel job

Date: 2026-10-03

## Problem

BuildBuddy's remote cache (#468) cut the warm "Build Bazel targets" step by
roughly 60-85% (e.g. Linux 23 min -> ~5 min, Windows 20 -> 3-8 min, macOS
10.6 -> 4-5 min). Cold runs, where a flag or toolchain change invalidates the
cache, see no benefit. The clang-tidy step is now the largest uncached cost:
0.8-2.2 min before BuildBuddy, 3.4-22 min after, with Windows at 19-22 min on
wide-scope PRs. (The post-BuildBuddy sample is small and confounded by the
wide-scope #471/#472, so treat the absolute numbers as indicative.)

Why it cannot use the cache: `scripts/clang_tidy_runner.py` runs via
`bazel run`, outside Bazel's action graph. Consequently:

- every in-scope translation unit is re-analyzed on every run;
- it runs serially after build and test, on the job's critical path;
- it runs on all three OSes, although most code is portable;
- `--changed` maps a changed header only to co-located sources.

## Goals

- PR-time tidy cost is bounded by the Linux job; Windows and macOS shrink to
  their platform-specific sources.
- No translation unit loses analysis (guarded, see Part 2).
- We gather the data needed to decide later on a fast/deep check tier split or
  a Bazel-aspect (cached per-file) design.

## Non-goals

- Bazel aspect / per-file cached tidy actions (follow-up, informed by Part 1).
- Remote execution.
- Splitting checks into fast and deep tiers (follow-up, informed by Part 1).
- Changing `.clang-tidy` checks.

## Part 1: Profiling

The runner measures itself.

- **Phase timers.** Print the duration of prebuild, compile-DB refresh,
  filtering and analysis, plus the translation-unit count.
- **`--profile` flag.** Adds `-enable-check-profile` to the `run-clang-tidy`
  command. `run-clang-tidy` has no `-store-check-profile` passthrough, so there
  is no per-file JSON; the aggregated per-check report is what we get.
- **Aggregation.** The runner parses that report (summing wall time per check
  across however many tables the LLVM version prints) and writes a markdown
  summary to `$GITHUB_STEP_SUMMARY`: top 15 checks by total time and the phase
  timings. Per-translation-unit ranking is out of scope.
- **CI usage.** Profiling is on for every CI tidy run (negligible cost), so
  data accumulates.
- **Tests.** Unit tests for aggregation and flag plumbing, alongside
  `scripts/clang_tidy_runner_test.py`.

## Part 2: One full platform, scoped runs elsewhere

- **Scope manifest** (`.clang-tidy-scope.toml`). Lists the path prefixes of
  Windows-exclusive code, and the platform-gated packages that the full Linux
  run already covers. An audit of every `target_compatible_with` found no
  macOS-exclusive code, so **there is no macOS tidy job**: Linux covers
  everything except Windows-exclusive code.
- **Per-OS behavior.** Linux analyzes everything in its compile DB. Windows
  analyzes only translation units under the manifest's Windows prefixes.
- **Narrower prebuild.** Scoped jobs build only their prefixes' targets, so
  Windows stops building `//...` only to materialize generated headers. The
  Hedron compile-DB refresh cannot take target patterns at run time, so it still
  runs its aquery over `//...` (no compilation, cheap).
- **Coverage guard.** A runner test fails if any `BUILD.bazel` uses
  `target_compatible_with` outside the manifest's prefixes, so a new
  platform-gated package cannot silently end up unanalyzed. Same style as the
  repo's existing build-graph guards.

**Accepted trade-off.** Portable code is no longer tidied under the Windows and
macOS toolchains (the QByteArray ambiguity fixed in #474 was found that way).
The per-OS build with warnings as errors remains the safety net. macOS loses
tidy entirely; two files with `__APPLE__` guards
(`qt_calibration_interaction_test.cpp`, `mock_openport.h`) are no longer
analyzed under macOS defines.

## Part 3: Tidy as a parallel job

- New `clang-tidy` job in `pr.yml`: matrix over Linux and Windows,
  `needs: pre-commit`, same setup as the `bazel` job (Qt, LLVM, BuildBuddy),
  `timeout-minutes: 30`, running `//:clang_tidy_report_changed`.
- The `bazel` job drops its tidy step; its critical path becomes build + test.
- **Cold-cache cost.** With a cold cache the tidy job prebuilds concurrently
  with the `bazel` job. Wall-clock is unaffected; runner-minutes rise on those
  runs. Accepted for now; the Linux job warms the cache for the others.

## Slicing

Three stacked PRs via `gh stack`:

1. Profiling (Part 1).
2. Scope manifest and guard (Part 2).
3. Job split (Part 3).

Part 1 lands first so its data confirms the choices in Parts 2 and 3.

## Risks

- The prebuild may be slower than assumed if the remote cache does not cover
  generated headers; Part 1's phase timers will show this.

## Success criteria

- Windows and macOS tidy jobs run only platform-specific translation units.
- PR critical path is max(bazel job, Linux tidy job) rather than their sum.
- The coverage guard passes, and every translation unit is analyzed on at least
  one OS.
