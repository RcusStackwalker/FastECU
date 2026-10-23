# Sonar pipeline: native Bazel caching

Date: 2026-10-23
Status: design agreed (grilling session), not started

## Goal

Reduce the SonarCloud job's reliance on internet downloads (flakiness) and its
wall time, using native Bazel mechanisms backed by the existing BuildBuddy
remote cache and remote downloader.

## Baseline (successful PR run, 2026-10-23, 5m13s total)

| Step | Time | Notes |
|---|---|---|
| Coverage (`scripts/coverage-local.sh`) | 3m09s | 160s Bazel elapsed, 11s critical path; 320 tests always rerun (`--nocache_test_results`) |
| Scan | 61s | ~15s downloads: scanner CLI ~6s, JRE ~8s, plugins ~3s |
| Setup (checkout, setup-bazel, apt) | ~45s | apt ~11s |

No scan retry fired in the last 40 PR runs; recent Sonar failures were all
quality-gate verdicts.

Coverage today is produced outside Bazel's cache (profraw written via
`--sandbox_writable_path`, merged by shell), so only a same-run rerun reuses it,
via a `run_id`-keyed `actions/cache` and the `coverage_artifact` export/import.

## Decisions

1. **Native `bazel coverage`.** `bazel coverage --config=coverage //...` with
   `BAZEL_USE_LLVM_NATIVE_COVERAGE=1` (plus `BAZEL_LLVM_COV` /
   `BAZEL_LLVM_PROFDATA`) and `--combined_report=lcov`. Per-test `coverage.dat`
   is a cacheable test result; the combined report is a Bazel coverage-report
   action at `bazel-out/_coverage/_coverage_report.dat`. The `llvm-cov show`
   report format and `sonar.cfamily.llvm-cov.reportPath` are dropped.
   - Keep `--dynamic_mode=off` and `-ffile-compilation-dir=.` unless proven
     unnecessary; drop the hand-written `-fprofile-instr-generate` /
     `-fcoverage-mapping` copts/linkopts that `bazel coverage` supplies.
2. **Instrumentation scope.** `build:coverage
   --instrumentation_filter=^//(src|apps|resources/shared)[/:]` and
   `--instrument_test_targets=false`. `sonar.coverage.exclusions` stays as the
   Sonar-side filter.
3. **Cached test results.** Drop `--nocache_test_results`, matching every other
   CI leg. Accepted cost: a flaky test's cached pass (and coverage) persists
   until its inputs change.
4. **Conversion step.** New `py_binary` `//tools/sonar:coverage_report`, run via
   `bazel run` after `bazel coverage` (the combined report cannot be an input to
   another build action). It:
   - reads the combined lcov report;
   - drops paths matching today's `coverage_ignore_regex`;
   - keeps paths checkout-relative (the generic format resolves them against
     `sonar.projectBaseDir`; revised during planning) and drops any path that is
     absolute or does not name an existing file under the checkout;
   - fails if fewer than 100 files resolve to existing files (the
     `MIN_RESOLVABLE_FILES` floor, carried over);
   - writes `coverage/sonar-coverage.xml` (Sonar generic coverage format,
     including branch data from lcov `BRDA` where present);
   - writes a human/agent-readable `coverage/coverage-summary.txt`.
   Tested with a small lcov fixture against byte-exact expected XML.
   `sonar-project.properties` switches to `sonar.coverageReportPaths`.
5. **Delete `scripts/coverage-local.sh`.** `scripts/nloc_loop.py` runs
   `bazel coverage --config=coverage //...` directly:
   - `parse_coverage` reads lcov (`SF:` / `DA:`) instead of `llvm-cov show`;
   - test-case counting reads every target's `test.xml` (cached or not) instead
     of the mtime-since-start filter, which relied on uncached results.
   Update `docs/tooling.md` (lines ~222, ~395) and
   `.claude/skills/hill-climbing-nloc/SKILL.md` (~146).
6. **Delete the rerun machinery.** The `run_id`-keyed `actions/cache`
   restore/save, the export/import steps, `tools/sonar/coverage_artifact.py`
   and its test. A rerun is a BuildBuddy cache hit.
7. **Bazel-owned scanner downloads.**
   - Scanner CLI: pinned `http_archive` (sonar-scanner-cli 8.1.0.6389, the
     version the action currently fetches) with sha256; served via BuildBuddy's
     remote downloader on hit.
   - JRE: the `linux-x64` scanner zip bundles its own JRE (`jre/bin/java`,
     verified 2026-10-23), so `sonar.scanner.skipJreProvisioning=true` makes the
     scanner use it; no rules_java dependency is needed. (Revised from
     `remotejdk` during planning.)
   - Analyzer plugins: server-chosen, not pinnable; `actions/cache` on
     `~/.sonar/cache` with a weekly-rotating key.
8. **Scan launcher.** `bazel run //tools/sonar:scan` replaces
   `sonarqube-scan-action`. Never cached (network, side effects). The one
   in-place retry and `sonarqube-quality-gate-action` stay.
   `--remote_download_regex=.*/_virtual_includes/.*` moves to the step that
   needs the Qt headers (`//:sonar_inputs`).
9. **Cache backend.** BuildBuddy only; no `--disk_cache` in `actions/cache`.
10. **Out of scope.** apt installs (`clang`/`llvm`, Qt runtime libs, ~11s).
    Replacing them means a hermetic Bazel C++ toolchain on every leg —
    follow-up.

## Delivery

A `gh stack` of three PRs, each green on its own:

1. Native coverage: decisions 1–6 (Bazel config, `coverage_report` + fixture
   test, properties switch, `nloc_loop` lcov parser and test counting,
   deletions, docs/skill updates, `sonar_workflow_test` updates).
2. Bazel-owned scanner: decisions 7–8.
3. ADR `docs/adr/0027-use-native-bazel-coverage-for-sonar.md` (reverses the
   rationale at `bazel/coverage.bazelrc:4`; records Bazel-owned scanner/JRE and
   why plugins stay on `actions/cache`) and the `coverage.bazelrc` comment
   rewrite.

## Qualification gate (PR 1)

PR 1's Sonar run must report overall coverage within ±1 percentage point of
master's latest figure, and new-code coverage must be non-zero on a change that
touches a `.cpp`. Both figures go in the PR body.

## Expected effect

- Coverage step: from ~3m on every commit to roughly the cost of the changed
  tests plus cache downloads; reruns near-free.
- Scan: ~14s of origin downloads (CLI + JRE) replaced by cached repository
  fetches; plugins served from `actions/cache`.
