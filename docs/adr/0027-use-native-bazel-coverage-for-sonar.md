# ADR 0027: Use Native Bazel Coverage for Sonar

## Status

Accepted and implemented by `bazel/coverage.bazelrc`,
`//tools/sonar:coverage_report`, `//tools/sonar:scan`, and the PR workflow's
SonarCloud job.

## Context

The SonarCloud job produced coverage outside Bazel's cache.
`scripts/coverage-local.sh` ran `bazel test --nocache_test_results` with the
profile files written through `--sandbox_writable_path`, merged them in shell,
and rendered an `llvm-cov show` report. On a successful PR run that step took
3m09s of a 5m13s job, with all 320 tests rerunning on every commit. Only a
rerun of the same workflow run could reuse the result, through a `run_id`-keyed
`actions/cache` and a coverage export/import pair.

The scan step also fetched the scanner CLI and a JRE from their origins on every
run, about 15 seconds of downloads that the BuildBuddy remote cache and remote
downloader could serve instead. Analyzer plugins are a separate case:
SonarCloud chooses their versions per project, so they cannot be pinned.

## Decision

- Coverage comes from native `bazel coverage --config=coverage //...` with LLVM
  source-based coverage and `--combined_report=lcov`. Per-test `coverage.dat`
  files are cacheable test results, and test results are cached like on every
  other CI leg (no `--nocache_test_results`).
- Instrumentation is limited to first-party code
  (`--instrumentation_filter=^//(src|apps|resources/shared)[/:]`, test targets
  not instrumented).
- `//tools/sonar:coverage_report` converts the combined lcov report to Sonar's
  generic coverage format (`sonar.coverageReportPaths`) and writes a readable
  summary. It is a `bazel run` step because the combined report cannot be an
  input to another build action. It keeps the existing ignore list and the
  resolvable-file floor.
- Bazel owns the scanner: a pinned `http_archive` of sonar-scanner-cli, whose
  bundled JRE is used (`sonar.scanner.skipJreProvisioning=true`, so there is no
  `rules_java` dependency), launched by `bazel run //tools/sonar:scan`. The scan
  itself is never cached.
- Analyzer plugins stay on `actions/cache` for `~/.sonar/cache` with a
  weekly-rotating key, because their versions are server-chosen.
- The rerun machinery (the `run_id` cache, the export/import steps and
  `tools/sonar/coverage_artifact.py`) is deleted. A rerun is a BuildBuddy hit.

This supersedes the rationale in the old `coverage.bazelrc` header for hand-set
profile flags and uncached tests. It does not supersede another ADR.

## Alternatives

- Keep `scripts/coverage-local.sh` and add an `actions/cache` keyed on source
  hashes. Rejected: it keeps coverage outside Bazel, needs a hand-built key that
  must track every input, and still reruns all tests on a miss.
- Provision the scanner JRE with `rules_java` `remotejdk`. Rejected: the
  scanner archive already bundles a JRE, so this adds a dependency for nothing.

## Consequences

- The `llvm-cov show` report format, `sonar.cfamily.llvm-cov.reportPath` and
  `scripts/coverage-local.sh` are gone. The NLOC loop's coverage guards, which
  ran `bazel coverage` and read lcov, were retired by ADR 0024.
- Cached test results now apply to coverage: a flaky test's cached pass, and
  its coverage, persists until its inputs change.
- `--experimental_generate_llvm_lcov` is needed on every host. Only
  `--experimental_use_llvm_covmap` and the `xcrun -f` paths of `llvm-profdata`
  and `llvm-cov` (passed as `--repo_env`) are macOS-specific.
- SonarCloud's overall and new-code `coverage` now include branch conditions
  (from lcov `BRDA`), which makes the new-code gate stricter for branchy code.
  On PR #811 overall `coverage` went from 84.1 (lines only) to 83.1 (lines plus
  14500 conditions at 79.2%) while `line_coverage` went from 84.1 to 84.3.
  Dropping the branch attributes in `generic_xml` is the one-line way back.
- The scan step no longer receives `GITHUB_TOKEN`.
- Plugins are still downloaded after a cache miss, at most weekly.
- The apt installs (`clang`, `llvm`, Qt runtime libraries, about 11 seconds)
  remain. Removing them needs a hermetic Bazel C++ toolchain on every CI leg and
  is a follow-up.
