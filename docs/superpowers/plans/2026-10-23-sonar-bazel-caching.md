# Sonar Native Bazel Caching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the SonarCloud job's coverage a BuildBuddy-cached `bazel coverage` product and its scanner a Bazel-pinned download, cutting wall time and origin downloads.

**Architecture:** `bazel coverage --config=coverage //...` (LLVM native coverage, combined lcov report) replaces `scripts/coverage-local.sh` for CI and for `nloc_loop`. A `bazel run //tools/sonar:coverage_report` step converts the lcov report to Sonar's generic coverage XML. A pinned `http_archive` of sonar-scanner-cli (bundled JRE) is launched by an uncached `bazel run //tools/sonar:scan`; analyzer plugins stay on `actions/cache`.

**Tech Stack:** Bazel (bzlmod), rules_python, Python 3.14 stdlib, GitHub Actions, BuildBuddy, SonarCloud, `gh stack`.

**Spec:** `docs/superpowers/specs/2026-10-23-sonar-bazel-caching-design.md`

## Global Constraints

- Cache backend is BuildBuddy only; no `--disk_cache`, no `actions/cache` for Bazel outputs.
- `--nocache_test_results` must not appear in any coverage path.
- Instrumentation: `--instrumentation_filter=^//(src|apps|resources/shared)[/:]`, `--noinstrument_test_targets`.
- Keep `--dynamic_mode=off` and `--copt=-ffile-compilation-dir=.` in `build:coverage`.
- Resolvable-file floor: 100 (`MIN_RESOLVABLE_FILES`), carried from `coverage-local.sh`.
- Ignore regex carried verbatim from `coverage-local.sh`: `(^|/)(tests|hexedit)/|(^|/)(moc_|qrc_|ui_)|\.moc$|rep_.*_replica\.h|(^|/)Qt/[0-9][^/]*/|/Applications/|/opt/homebrew/|/Library/Developer/|bazel-out/|external/`
- Scanner: sonar-scanner-cli `8.1.0.6389` linux-x64, sha256 `bb8f709f9cb73352f8d1260a3b3c506c0f41146754bc630762c126d795499d0b`, strip prefix `sonar-scanner-8.1.0.6389-linux-x64`, URL `https://binaries.sonarsource.com/Distribution/sonar-scanner-cli/sonar-scanner-cli-8.1.0.6389-linux-x64.zip`.
- Plugin cache: `~/.sonar/cache`, key rotates weekly (ISO week).
- Scan never cached; one in-place retry; `sonarqube-quality-gate-action` unchanged.
- PR 1 qualification: overall coverage within ±1 percentage point of master's latest; new-code coverage non-zero on a `.cpp`-touching change; both figures in the PR body.
- Delivery: a `gh stack` of three PRs (`gh stack init --base master <b1> <b2> <b3>`, `gh stack submit --auto`), each green on its own. Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

- A base snapshot taken with the old `llvm-cov show` format compared against a new lcov head snapshot must be refused with "retake the base snapshot", not silently compared (Task 2).
- A remote-cache-hit test whose `test.xml` was not downloaded (BEP gives a non-`file://` URI) must fail the snapshot, not undercount test cases (Task 2).
- lcov `SF:` records that are absolute, under `external/`/`bazel-out/`, generated (`moc_*`), or name a file that does not exist must be dropped and never emitted; if fewer than 100 survive the tool fails (Task 1).
- Sonar must still see the Qt headers under `_virtual_includes` once the coverage build no longer carries `--remote_download_regex`; otherwise it misparses Qt's `emit` (Task 3, verification step).
- Replacing `sonarqube-scan-action` must not turn PR analyses into branch analyses: the scanner log must still report a pull-request analysis with the PR key (Task 5, verification step).

---

## PR 1 — branch `sonar/native-coverage`

### Task 1: lcov → Sonar generic coverage converter

**Files:**
- Create: `tools/sonar/coverage_report.py`
- Create: `tools/sonar/tests/coverage_report_test.py`
- Modify: `tools/sonar/BUILD.bazel` (add `py_binary` `coverage_report`, tags `manual`; `py_test` `coverage_report_test`, size small, `imports = ["."]`, same Windows-incompatible `select` as `sonar_inputs_test`)

**Interfaces:**
- Produces:
  - `@dataclass(frozen=True) class FileCoverage: lines: dict[int, int]; branches: dict[int, tuple[int, int]]` (line → hits; line → (to_cover, covered))
  - `parse_lcov(text: str) -> dict[str, FileCoverage]` — `SF:`, `DA:<line>,<hits>[,…]`, `BRDA:<line>,<block>,<branch>,<taken|->`, `end_of_record`; repeated `SF` for one path merges (hits summed per line, branch counts summed per line, a branch is covered if taken > 0).
  - `select_files(records, root: Path, ignore: re.Pattern[str]) -> dict[str, FileCoverage]` — drops ignored, absolute, and non-existent paths.
  - `generic_xml(records) -> str` — `<coverage version="1">`, files and lines sorted, `<lineToCover lineNumber= covered= [branchesToCover= coveredBranches=]/>`; branch attributes only when the line has `BRDA` data.
  - `summary(records) -> str` — one line per file `path covered/total`, then `TOTAL covered/total pct%`.
  - CLI: `coverage_report --lcov PATH --root PATH --output-dir PATH [--min-files N=100]`; writes `sonar-coverage.xml` and `coverage-summary.txt` into `--output-dir`; exits 1 with a message naming the count and the first 5 dropped paths when fewer than `--min-files` files survive.

- [ ] **Step 1: Write failing tests** in `coverage_report_test.py` (use `tempfile.TemporaryDirectory` as root, create the referenced source files):
  - `test_parses_lines_and_branches`: lcov with `SF:src/a.cpp`, `DA:1,3`, `DA:2,0`, `BRDA:1,0,0,1`, `BRDA:1,0,1,-` → `{"src/a.cpp": FileCoverage({1: 3, 2: 0}, {1: (2, 1)})}`.
  - `test_merges_repeated_records`: two records for `src/a.cpp` with `DA:1,1` and `DA:1,2` → `lines == {1: 3}`.
  - `test_drops_ignored_absolute_missing`: records for `src/a.cpp` (exists), `/usr/include/x.h`, `external/qt/q.h`, `src/moc_w.cpp`, `src/tests/t.cpp`, `src/gone.cpp` (missing) → only `src/a.cpp` kept.
  - `test_generic_xml_is_exact`: one file, lines 1 (hits 3, branches 2/1) and 2 (hits 0) → exactly
    `<coverage version="1">\n  <file path="src/a.cpp">\n    <lineToCover lineNumber="1" covered="true" branchesToCover="2" coveredBranches="1"/>\n    <lineToCover lineNumber="2" covered="false"/>\n  </file>\n</coverage>\n`
  - `test_main_fails_below_floor`: `main([...,"--min-files","2"])` with one surviving file → returns 1, stderr contains `1 of` and the dropped path; no XML written.
  - `test_main_writes_both_outputs`: with `--min-files 1` → returns 0; both files exist; summary ends with `TOTAL 1/2 50.0%`.
- [ ] **Step 2:** `bazel test //tools/sonar:coverage_report_test` → FAIL (module missing).
- [ ] **Step 3:** Implement `coverage_report.py` with the interfaces above; `IGNORE` compiled from the Global Constraints regex; write the XML with string building (no third-party deps); `main(argv: Sequence[str] | None = None) -> int`.
- [ ] **Step 4:** `bazel test //tools/sonar:coverage_report_test` → PASS.
- [ ] **Step 5:** Commit `feat(sonar): convert Bazel lcov coverage to Sonar generic format`.

### Task 2: Native `bazel coverage` config, `nloc_loop` on lcov, delete `coverage-local.sh`

**Files:**
- Modify: `bazel/coverage.bazelrc`
- Modify: `scripts/nloc_loop.py:47,334-354,374-403,510,552-572`
- Modify: `scripts/tests/nloc_loop_test.py:280-325` (+ new tests)
- Delete: `scripts/coverage-local.sh`
- Modify: `docs/tooling.md:220-225,395-400`, `.claude/skills/hill-climbing-nloc/SKILL.md:144-149`

**Interfaces:**
- Produces (used by Task 3): the coverage command `bazel coverage --config=coverage //...` writes the combined report to `$(bazel info --config=coverage output_path)/_coverage/_coverage_report.dat`.
- Produces in `nloc_loop.py`:
  - `parse_coverage(lcov: str, root: Path) -> dict[str, FileCoverage]` (existing `FileCoverage` type; `DA` hits > 0 → covered, = 0 → uncovered, executable = count of `DA` lines; `SF` paths relative; absolute paths outside root raise `NlocLoopError("... outside ...")` as today).
  - `COVERAGE_FORMAT = "lcov"`; `Snapshot` gains `format: str` (saved as `"format"`; loading a snapshot without the key yields `"llvm-cov-show"`).
  - `check_guards` raises `NlocLoopError` containing `retake the base snapshot` when `base.format != head.format`.
  - `test_xml_paths(bep_json: str) -> list[Path]` — from `testResult` events' `testActionOutput` entries named `test.xml`; a non-`file://` URI raises `NlocLoopError` naming the label.
  - `run_bazel_coverage(root: Path) -> tuple[str, list[str]]` replaces `run_coverage_local` (`RunCoverage` signature unchanged).

- [ ] **Step 1: Rewrite `bazel/coverage.bazelrc`'s coverage block.** Remove the four hand-written `-fprofile-instr-generate`/`-fcoverage-mapping` copt/linkopt lines; keep `--compilation_mode=dbg`, `CC/CXX=clang`, `--dynamic_mode=off`, `-ffile-compilation-dir=.`; add:
  ```
  build:coverage --repo_env=BAZEL_USE_LLVM_NATIVE_COVERAGE=1
  build:coverage --repo_env=GCOV=llvm-profdata
  build:coverage --repo_env=BAZEL_LLVM_COV=llvm-cov
  coverage:coverage --combined_report=lcov
  coverage:coverage --experimental_fetch_all_coverage_outputs
  coverage:coverage --instrumentation_filter=^//(src|apps|resources/shared)[/:]
  coverage:coverage --noinstrument_test_targets
  build:sonar_inputs --remote_download_regex=.*/_virtual_includes/.*
  ```
  Leave the existing comments that explain kept flags; the header comment is rewritten in Task 6.
- [ ] **Step 2: Verify on Linux and macOS.** Run `bazel coverage --config=coverage //...` (on macOS add `--repo_env=GCOV=$(xcrun -f llvm-profdata) --repo_env=BAZEL_LLVM_COV=$(xcrun -f llvm-cov)`). Expected: exit 0; `_coverage_report.dat` exists, contains `SF:src/` records for library sources (not only tests), and no `SF:` beginning with `/` for `src/` files. Rerun: Bazel reports the tests `(cached)`. Then `bazel run //tools/sonar:coverage_report -- --lcov <dat> --root "$PWD" --output-dir coverage` → exit 0 with ≥ 100 files. If the combined report is empty or execroot-absolute, stop and fix the config before continuing.
- [ ] **Step 3: Write failing tests** in `nloc_loop_test.py`: replace the `coverage_report` helper with an lcov builder (`SF:`/`DA:`/`end_of_record`), port `test_covered_and_executable_lines` (`{1:3, 2:0, 3:10}` → covered `{1,3}`, executable 3, uncovered `{2}`) and `test_rejects_sections_outside_the_root`; drop `test_ignores_instantiation_subviews`; add:
  - `test_snapshot_without_format_loads_as_llvm_cov_show` and `test_round_trips_lcov_format`.
  - `test_guards_refuse_mixed_formats` → `assertRaisesRegex(NlocLoopError, "retake the base snapshot")`.
  - `test_test_xml_paths_from_bep` (two `testResult` events, one `file://` test.xml each → both paths) and `test_test_xml_paths_rejects_remote_uri` (`bytestream://…` → raises, message contains the label).
  - Update the existing guard test at `:607` only as far as the report string format.
- [ ] **Step 4:** `bazel test //:nloc_loop_test` → FAIL.
- [ ] **Step 5: Implement** the `nloc_loop.py` interfaces. `run_bazel_coverage` runs `bazel coverage --config=coverage --remote_download_regex=.*/test\.xml$ --build_event_json_file=<.nloc-loop/coverage/bep.json> //...` (on Darwin append the two `xcrun -f` `--repo_env` flags from Step 2), fails with the last 40 output lines as today, reads `bazel info --config=coverage output_path` + `/_coverage/_coverage_report.dat`, and returns `(lcov text, [p.read_text() for p in test_xml_paths(bep)])`. Snapshots taken by `guards snapshot` record `format=COVERAGE_FORMAT`.
- [ ] **Step 6:** `bazel test //:nloc_loop_test` → PASS. Then `python3 scripts/nloc_loop.py guards snapshot` on a clean HEAD → writes a snapshot with `"format": "lcov"` and a non-zero test-case count.
- [ ] **Step 7: Delete `scripts/coverage-local.sh`; update docs.** `docs/tooling.md:222` local reproduction becomes `bazel coverage --config=coverage //...` then `bazel run //tools/sonar:coverage_report -- --lcov "$(bazel info --config=coverage output_path)/_coverage/_coverage_report.dat" --root "$PWD" --output-dir coverage`; `:395` says `guards snapshot` runs `bazel coverage` and reads test.xml from the build event file, and that old-format snapshots must be retaken. In the SKILL.md TODO comment replace the `coverage-local.sh … --nocache_test_results` parenthetical with `bazel coverage` (test results are cached, so a profile-losing crash now shows as a failed test, not a missing profile). `git grep coverage-local -- ':!docs/superpowers' ':!docs/reference'` → only `.github/workflows/sonar.yml` (fixed in Task 3).
- [ ] **Step 8:** Commit `feat(coverage): produce coverage with native bazel coverage`.

### Task 3: Workflow cutover and rerun-machinery deletion

**Files:**
- Modify: `.github/workflows/sonar.yml`
- Modify: `sonar-project.properties` (replace `sonar.cfamily.llvm-cov.reportPath=coverage/llvm-cov.report` with `sonar.coverageReportPaths=coverage/sonar-coverage.xml`)
- Modify: `tools/sonar/tests/sonar_workflow_test.py`
- Delete: `tools/sonar/coverage_artifact.py`, `tools/sonar/tests/coverage_artifact_test.py`, their two targets in `tools/sonar/BUILD.bazel`

**Interfaces:**
- Consumes: Task 1 CLI, Task 2 coverage command and report path.
- Produces (Task 5 extends): step ids `coverage` (`bazel coverage --config=coverage //...`) and `coverage_report` (`bazel run --config=coverage //tools/sonar:coverage_report -- --lcov "$(bazel info --config=coverage output_path)/_coverage/_coverage_report.dat" --root "$GITHUB_WORKSPACE" --output-dir "$GITHUB_WORKSPACE/coverage"`), before the unchanged `Prepare configured scanner inputs` step.

- [ ] **Step 1: Rewrite `sonar_workflow_test.py`** (failing against today's workflow):
  - `test_one_job_covers_converts_prepares_and_scans`: order `coverage` < `coverage_report` < `Prepare configured scanner inputs` < `sonarcloud` < `sonarcloud_retry` < quality gate.
  - `test_coverage_is_native_and_cached`: the `coverage` step's run contains `bazel coverage --config=coverage //...`; no step's run contains `coverage-local.sh`, `nocache_test_results`, or `coverage_artifact`; no step uses `actions/cache` with `github.run_id` in its key.
  - `test_scanner_reads_generic_coverage`: properties contain `sonar.coverageReportPaths=coverage/sonar-coverage.xml` and not `llvm-cov.reportPath`.
  - Keep `test_it_keeps_the_caller_interface`, `test_the_scanner_reads_the_aspect_database_and_hedron_is_not_used`, `test_a_transient_scan_failure_is_retried_once_before_the_gate`; drop `test_both_boundaries_…`, `test_rerun_reuses_…`, `test_coverage_is_executed_once_per_fresh_attempt`.
- [ ] **Step 2:** `bazel test //tools/sonar:sonar_workflow_test` → FAIL.
- [ ] **Step 3: Edit `sonar.yml`**: remove `coverage_cache`, `coverage_export`, `coverage_save`, `coverage_import` and the `EXTRA_BAZEL_ARGS` env; add the two steps from Interfaces; update the job-level comment to say a rerun is a BuildBuddy cache hit. Update properties; delete the `coverage_artifact` files and targets.
- [ ] **Step 4:** `bazel test //tools/sonar/... //:sonar_scope_test --config=release` → PASS; `git grep -n coverage_artifact` → no hits outside `docs/superpowers`.
- [ ] **Step 5:** Commit `ci(sonar): cut over to native bazel coverage and drop rerun cache`.

### Task 4: PR 1 qualification

- [ ] **Step 1:** Push `sonar/native-coverage`; `gh stack init --base master sonar/native-coverage` and `gh stack submit --auto` (later branches are added in Tasks 5–6).
- [ ] **Step 2: Check the run.** In the SonarCloud job log: the `coverage` step shows remote-cache hits for test actions on a rerun (`gh run rerun --job`); Sonar logs no parse errors mentioning `qobjectdefs.h` or `emit`; the sensor log shows the generic coverage report imported with no "unknown file" warnings.
- [ ] **Step 3: Compare figures.**
  `curl -s -u "$SONAR_TOKEN:" 'https://sonarcloud.io/api/measures/component?component=RcusStackwalker_FastECU&metricKeys=coverage'` (master) and the same with `&pullRequest=<N>&metricKeys=coverage,new_coverage`. Expected: |PR − master| ≤ 1.0 and `new_coverage` > 0 when the PR touches a `.cpp` (if PR 1 touches none, push a throwaway commit touching a covered `.cpp` comment line to a scratch branch based on it, read the figure, delete the branch). Record both figures and the before/after SonarCloud job durations in the PR body.

## PR 2 — branch `sonar/bazel-scanner` (stacked on PR 1)

### Task 5: Bazel-owned scanner, launcher, plugin cache

**Files:**
- Modify: `MODULE.bazel` (after the `http_file` block: `http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")`, repo `sonar_scanner_cli` with the Global Constraints URL/sha256/strip_prefix and `build_file_content` exporting `bin/sonar-scanner` and a `filegroup` `files` of `glob(["**"])`, public visibility)
- Create: `tools/sonar/sonar_scan.py`, `tools/sonar/tests/sonar_scan_test.py`
- Modify: `tools/sonar/BUILD.bazel` (`py_binary` `scan`, main `sonar_scan.py`, `data = ["@sonar_scanner_cli//:files"]`, deps `@rules_python//python/runfiles`, tags `manual`; `py_test` `sonar_scan_test`)
- Modify: `.github/workflows/sonar.yml`, `tools/sonar/tests/sonar_workflow_test.py`, `docs/tooling.md` (local reproduction uses `bazel run //tools/sonar:scan -- -Dsonar.token=$SONAR_TOKEN`)

**Interfaces:**
- Produces: `scan_command(scanner: Path, args: Sequence[str]) -> list[str]` returning `[str(scanner), "-Dsonar.scanner.skipJreProvisioning=true", *args]`; `main(argv) -> int` resolves `sonar_scanner_cli/bin/sonar-scanner` via runfiles, `chdir`s to `BUILD_WORKSPACE_DIRECTORY` (error if unset), and `os.execv`s the command; environment (incl. `SONAR_TOKEN`, `GITHUB_*`) passes through unchanged.

- [ ] **Step 1: Failing tests.** `sonar_scan_test.py`: `test_command_disables_jre_provisioning_first` (exact list for `Path("/s/bin/sonar-scanner"), ["-X"]`); `test_main_requires_workspace_directory` (no `BUILD_WORKSPACE_DIRECTORY` → returns 2, message names the variable). `sonar_workflow_test.py`: `test_scan_is_a_bazel_launcher` — `sonarcloud` and `sonarcloud_retry` have identical `run` `bazel run //tools/sonar:scan` and identical `env` (`SONAR_TOKEN` only), no `uses`; `test_plugins_are_cached_weekly` — a restore step with `path: ~/.sonar/cache` whose key contains `steps.week.outputs.week`, a matching save step with `if: inputs.cache-save`, the `week` step before the restore; update `test_a_transient_scan_failure…` to compare `run` instead of `uses`.
- [ ] **Step 2:** `bazel test //tools/sonar:sonar_scan_test //tools/sonar:sonar_workflow_test` → FAIL.
- [ ] **Step 3: Implement.** `sonar_scan.py` per Interfaces. In `sonar.yml`: step `id: week` (`echo "week=$(date -u +%G-%V)" >> "$GITHUB_OUTPUT"`); `actions/cache/restore` (same pinned SHA as today, `v6.1.0`) key `sonar-plugins-${{ runner.os }}-${{ steps.week.outputs.week }}`, `restore-keys: sonar-plugins-${{ runner.os }}-`; scan + retry as `run: bazel run //tools/sonar:scan` with `env: SONAR_TOKEN`; `actions/cache/save` with the same key, `if: inputs.cache-save && always()` placed after the retry. Add `MODULE.bazel` repo; run `bazel mod tidy` if it reports changes and update `MODULE.bazel.lock`.
- [ ] **Step 4:** Tests from Step 1 → PASS. `bazel run //tools/sonar:scan -- --version` locally (Linux runner or container) → prints scanner `8.1.0.6389` and the bundled Java version, with no download lines.
- [ ] **Step 5: CI check.** Push, `gh stack submit --auto`. In the job log: no `binaries.sonarsource.com` download, no `JRE provisioning` line; the scanner reports a pull-request analysis with the PR number (not a branch analysis); the second run of the week shows the plugin cache restored and `Load/download plugins` without downloads. Quality gate step behaves as before.
- [ ] **Step 6:** Commit `ci(sonar): run a Bazel-pinned scanner with cached plugins`.

## PR 3 — branch `sonar/adr` (stacked on PR 2)

### Task 6: ADR 0027 and config comment

**Files:**
- Create: `docs/adr/0027-use-native-bazel-coverage-for-sonar.md` (follow `docs/adr/README.md` format and the style of `0022`)
- Modify: `bazel/coverage.bazelrc:1-6` header comment

- [ ] **Step 1:** Write the ADR: context (uncached coverage, 3m09s of 5m13s baseline, rerun-only reuse, scanner origin downloads ~15s); decision (native `bazel coverage` with combined lcov → generic format; Bazel-pinned scanner with bundled JRE; plugins on `actions/cache` because SonarCloud chooses their versions); consequences (llvm-cov `show` format and `coverage-local.sh` gone; cached test results apply to coverage, so a flaky pass persists until inputs change; nloc snapshots must be retaken across the format change; apt installs remain a follow-up). It supersedes the rationale in the old `coverage.bazelrc` header, not an ADR.
- [ ] **Step 2:** Rewrite the `coverage.bazelrc` header to: the `coverage` config drives native `bazel coverage` (LLVM source-based coverage, combined lcov report) for SonarCloud and the nloc guards; see ADR 0027.
- [ ] **Step 3:** `bazel test --config=release //:sonar_scope_test` → PASS (docs don't affect scope, sanity only).
- [ ] **Step 4:** Commit `docs(adr): record native bazel coverage for sonar`; push; `gh stack init --base master sonar/native-coverage sonar/bazel-scanner sonar/adr` (if not already) and `gh stack submit --auto`. Hand-off mentions `gh stack merge` / `gh stack sync`.
