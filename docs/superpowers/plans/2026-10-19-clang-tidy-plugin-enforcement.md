# Clang-tidy Plugin Enforcement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce repository conventions through mandatory Bazel-built clang-tidy checks locally and in CI, with one wider analysis scope.

**Architecture:** Extend the existing extension, qualify its LLVM host before selecting platform packaging, and migrate YAML checks with diagnostic equivalence. Then unify analysis and add conservative assertion checks, cleaning existing findings before enabling each check.

**Tech Stack:** Bazel, LLVM/Clang clang-tidy C++ APIs, Python runner/fixtures, GoogleTest/GoogleMock, GitHub Actions.

**Spec:** [Approved design](../specs/2026-10-19-clang-tidy-plugin-enforcement-design.md).

## Draft implementation checkpoint

The native migration, unified runner, assertion-check implementations and initial
cleanup are available for review. On macOS the current snapshot passes all 31
native fixtures, the changed-file pre-commit gates and the full release test
run (294 passed, 20 platform-incompatible tests skipped). Earlier focused runs
also passed the 67 flash ECU/bound-attempt suites and 47 applicable
parameterized-case suites.

This remains a draft. Frozen full-scope zero-finding reports, fresh-database
migration equivalence, runtime measurements, Windows linked-host qualification,
remaining check enablement, enduring documentation and final independent review
are still required. Exploratory reports taken during cleanup are not final
qualification evidence. The original approved plan remains preserved in history.

## Global Constraints

- Supported local LLVM is CI's pinned release, currently 23.1.2; validate the actual selected installation and host compatibility.
- Equivalent enforcement on macOS/Linux locally and Linux/Windows CI, including Windows-exclusive code.
- Unified first-party `.h`/`.hpp` scope under `src/`, `apps/`, `tests/`, and `scripts/`, retaining generated/vendor exclusions.
- Checks use `fastecu-*`. Preserve existing diagnostics through migration, allowing the intentional prefix normalization.
- New checks provide diagnostics only; preserve the template check's existing fix-it.
- Fix existing violations before enabling each blocking check; no permanent baseline or routine inline suppressions.
- All analysis/check/configuration changes trigger full-scope analysis; required fixtures cannot skip silently.
- Bazel owns builds, tests, analysis and qualification actions. Preserve unrelated working-tree files and stage named paths.
- No warning opt-outs. Resolve the existing extension's suppression in accordance with ADR 0018.

## Review Focus

- Mixed LLVM installations: matching version strings alone must not authorize loading a binary from a different installation (Task 1/2).
- Assertion macros, callbacks and templates: diagnose the user expression once, without diagnosing GoogleTest internals (Tasks 3/7).
- Result mutation and aliases: a prior success assertion must not authorize access after state changes (Task 7).
- Partial analysis: exception entries outside changed-files scope must not be rejected as unused (Task 6).
- Ordinary app builds without LLVM: application/test loading must remain usable; analysis requests must fail clearly (Task 2).

## File and interface map

- `scripts/clang_tidy_plugin/`: existing module, migrated check classes, new assertion check classes, and shared `TestAssertionSupport.{h,cpp}` for macro origins and Result recognition.
- `bazel/llvm.bzl`, `MODULE.bazel`: explicitly selected installation and platform-qualified analysis artifacts.
- `scripts/clang_tidy_toolchain.py`: installation validation and launch description; public `resolve_analysis_host(llvm_root: Path, artifact: Path) -> AnalysisHost`, where `AnalysisHost` contains executable and immutable argument tuple.
- `scripts/clang_tidy_runner.py`, `bazel/clang_tidy.bzl`: consume that host, preserve existing report/fix/changed entry points, launch one pass after migration.
- `scripts/clang_tidy_checks_test.py`: real-dependency positive and counterexample fixtures, using exported diagnostic sets.
- `.clang-tidy-exceptions.yaml`: versioned exception records passed through `CheckOptions`; `scripts/clang_tidy_exceptions.py` validates schema/identities and full-scope usage.
- `scripts/clang_tidy_benchmark.py`: comparable scope and artifacts in fingerprints; migration diagnostic comparison support.
- `.github/workflows/pr.yml`, `.github/actions/llvm-linux/action.yml`: install the selected distribution, build/load checks and require fixtures.
- `docs/tooling.md`, `docs/coding-style.md`: delivered integration and convention boundaries.

## Task 1: Qualify platform packaging

**Files:** Create `scripts/clang_tidy_plugin/qualification/README.md` and `scripts/clang_tidy_plugin/qualification/smoke.cpp`; modify `scripts/clang_tidy_plugin/BUILD.bazel` for a Bazel-owned smoke target; inspect `bazel/llvm.bzl` and CI LLVM installers.

**Interfaces:** Produces a recorded packaging decision with artifact paths, host/compiler versions, exact build/load command and observed diagnostics for each supported platform. Later tasks consume this evidence, not assumptions about archive contents.

- [ ] Add a smoke source whose template parameter uses `class`; require exactly one `fastecu-template-typename` finding and its existing replacement.
- [ ] Run against each selected LLVM distribution; first confirm the required extension is unavailable or unsupported under the current integration.
- [ ] Inspect Windows headers, libraries and executable exports; build/load the existing extension using that installation. Record successes and exact failures.
- [ ] If Windows cannot load it, qualify a Bazel-built host with linked checks using the distribution's libraries. Record its dependencies/build cost. If those libraries are insufficient, stop for design review before adopting an LLVM source build or changing provisioning.
- [ ] Record macOS/Linux build/load results and verify ordinary application targets still load without LLVM.
- [ ] Commit qualification sources and evidence separately. No rollout claim without results from all required platforms.

## Task 2: Mandatory, validated build/run integration

**Files:** Modify `bazel/llvm.bzl`, `MODULE.bazel`, `scripts/clang_tidy_plugin/BUILD.bazel`, `bazel/clang_tidy.bzl`, `BUILD.bazel`, CI installers/workflow; create `scripts/clang_tidy_toolchain.py` and `scripts/clang_tidy_toolchain_test.py`.

**Interfaces:** Implement `AnalysisHost` and `resolve_analysis_host` from the map using Task 1's qualified platform packaging. Use explicit `FASTECU_LLVM_ROOT`; require clang-tidy and headers/libraries from that root. The existing Bazel analysis commands build/select artifacts automatically, without an optional-plugin bypass.

- [ ] Add unit cases `test_wrong_release_fails_before_launch`, `test_mixed_installation_fails`, `test_missing_artifact_fails`, and `test_paths_with_spaces`; assert actionable errors and zero analysis launches for invalid inputs.
- [ ] Run `bazel test //:clang_tidy_toolchain_test`; confirm new cases fail before implementation.
- [ ] Expose required LLVM headers as external/system headers; remove `-Wno-deprecated-this-capture` and fix remaining first-party warnings. Make installation discovery lazy enough that ordinary builds work without LLVM.
- [ ] Implement host validation, Bazel artifact/runfiles wiring and platform launch selection. Validate full identity/build compatibility through the qualified build/load smoke, not merely a major-version comparison.
- [ ] Make `//:clang_tidy_check_fixtures` require the extension; retain the old plugin-fixture label as an alias if useful to callers. Missing extension is a failure, never a skip.
- [ ] Run unit tests, build/load smoke and required fixtures on CI platforms; verify local macOS behavior. Commit named integration paths.

## Task 3: Migrate all nine YAML checks

**Files:** Modify `.clang-tidy`, `scripts/clang_tidy_plugin/FastecuTidyModule.cpp`, plugin BUILD, fixture/runner tests; create one `{CheckName}Check.{h,cpp}` pair per existing YAML check. Preserve their current comments and scope in code/tests.

**Interfaces:** Register `fastecu-redundant-address-dereference`, `fastecu-result-assertion`, `fastecu-container-assertion`, `fastecu-message-concatenation`, `fastecu-linear-search-loop`, `fastecu-any-all-index-loop`, `fastecu-count-if-loop`, `fastecu-accumulate-loop`, and `fastecu-transform-collect-loop`.

- [ ] For each check, run its current fixtures and capture the full applicable-scope diagnostic set before editing. Store comparison artifacts under ignored `reports/`, recording revision/toolchain/scope.
- [ ] Run each fixture with its new registered name; confirm the missing implementation fails.
- [ ] Port its matcher and callback, preserving message, severity, source anchor, exclusions and absence of fixes. Retain macro and dependent-template counterexamples, STL iterator variants and Result API exceptions.
- [ ] Run `bazel run --config=release //:clang_tidy_check_fixtures`; compare old/new full-scope diagnostics after normalizing only `custom-` and absolute path roots. Investigate every other difference; compare identical header scopes.
- [ ] Remove that check's YAML body only after parity passes; update name references and commit the independently verified check. Repeat for all nine before extending assertion behavior.

## Task 4: Clean and unify analysis scope

**Files:** Modify `.clang-tidy`, `scripts/clang_tidy_runner.py`, runner/output tests, `bazel/clang_tidy.bzl`, root BUILD; delete `scripts/clang_tidy_adapter.py` and its tests after removing callers. Modify only named source files with new findings.

**Interfaces:** One invocation per translation unit enables built-in and registered checks with the wider scope. Preserve report/fix/changed labels, annotations, deduplication, profile output, platform manifests and generated-header exclusions.

- [ ] Add runner cases `test_single_pass_uses_wide_scope`, `test_plugin_change_invalidates_full_scope`, `test_exception_change_invalidates_full_scope`, and `test_generated_headers_remain_excluded`; assert one analysis launch and correct selected translation units/filters.
- [ ] Run `bazel test //:clang_tidy_runner_test`; confirm new expectations fail.
- [ ] Run wider built-in analysis without switching the gate; fix all exposed findings, including naming in app/test headers. Do not suppress or broaden exclusions to avoid cleanup.
- [ ] Set the unified filter, merge analysis paths, remove adapter and custom-pass options/callers, and widen invalidation to plugin sources, build configuration, toolchain and exception configuration.
- [ ] Update benchmark/report phase expectations and fixtures so successful analysis still produces complete findings and supported fixes.
- [ ] Run runner/output/profile tests and full applicable report, require zero findings, then commit cleanup and integration in reviewable changes.

## Task 5: Migration and performance evidence

**Files:** Modify `scripts/clang_tidy_benchmark.py`, `scripts/clang_tidy_benchmark_test.py`; update `docs/tooling.md` commands and evidence instructions.

**Interfaces:** Benchmark fingerprints include plugin sources, selected artifact digest, LLVM identity, configuration, runner and scope. Diagnostic comparison normalizes path roots/name prefix only.

- [ ] Add `test_plugin_change_invalidates_fingerprint` and `test_diagnostic_comparison_preserves_message_and_location`; assert message/severity/location differences remain visible.
- [ ] Run `bazel test //:clang_tidy_benchmark_test`; confirm failures, implement the expanded fingerprint/comparison, then rerun to pass.
- [ ] Compare equivalent-scope YAML/native migration artifacts and fixed-scheduling before/after unified-scope runtime. Keep scope-widening cost distinct from native migration cost.
- [ ] Run repeated measurements on the same machine; report medians and raw inputs. Investigate regressions above 10 percent without introducing a wall-clock CI gate. Commit tooling and concise evidence links.

## Task 6: Central exceptions and shared assertion recognition

**Files:** Create `.clang-tidy-exceptions.yaml`, `scripts/clang_tidy_exceptions.py`, its Python tests, and plugin `TestAssertionSupport.{h,cpp}`; modify fixture runner, plugin BUILD, root BUILD and runner.

**Interfaces:** YAML schema is `version: 1` and `exceptions: [{check, file, symbol, rationale}]`; all fields are required strings. `file` is a normalized repository-relative path. `symbol` is a qualified helper signature or `Suite.Test` for GoogleTest bodies. Exact identity matching only; no wildcards or line numbers. Pass the path through the `fastecu.ExceptionsFile` CheckOption. Full-scope verification reports matched identities and fails unused entries.

- [ ] Add Python cases rejecting unknown schema versions/keys, empty rationales, absolute/traversing paths, duplicates and unused full-scope entries; require partial-scope runs to leave unrelated entries alone.
- [ ] Add C++ fixtures covering aliases of `std::expected<T, fastecu::Error>`, Status, unrelated expected error types, optional, qualified matchers, macros, lambdas and instantiated templates. Assert one user-site diagnostic and none from framework internals.
- [ ] Run focused Python tests and real-dependency fixtures; confirm new expectations fail.
- [ ] Implement schema validation/identity matching and shared Result/macro recognition; report malformed configuration before analysis. Add only justified exact exceptions, including matcher implementation handling and low-level API tests.
- [ ] Run focused tests and full-scope usage verification. Commit infrastructure with passing consumers.

## Task 7: Result-state safety and diagnostic conventions

**Files:** Extend `ResultAssertionCheck.{h,cpp}`; create `ResultAccessCheck.{h,cpp}` and register it; extend shared support/fixtures and fix named affected tests.

**Interfaces:** `fastecu-result-assertion` handles state assertions, direct error-field assertion patterns, appended detail and unauthorized `Not(IsOk())`; `fastecu-result-access` handles state-specific access within a function.

- [ ] Add fixtures requiring success before dereference/arrow, error before `.error()`, and acceptance after terminating guards or fatal `IsOkAnd`/`IsErrWith`. Require nonfatal expectations and wrong-state guards to leave access unproved.
- [ ] Add mutation/branch fixtures: reassignment, non-const reference escape, conditional exits, joins and loops. Require diagnostics when state is no longer proved; allow const observations and locally proved branches. Access through unknown aliases stays within the documented conservative boundary.
- [ ] Add fixtures for appended error detail, direct `.error().kind/detail` assertions, permitted API/matcher internals, and any-error assertions with/without exact central rationale.
- [ ] Run fixtures to establish failures; implement intraprocedural state reasoning using Clang CFG/dataflow facilities compatible with the pinned LLVM. Avoid claiming interprocedural completeness.
- [ ] Clean existing violations with matcher/guard changes. Full-scope report must be clean before enabling each new registered check in `.clang-tidy`.
- [ ] Run affected package tests and fixtures, then full report; commit checks and their cleanups in independently reviewable changes.

## Task 8: Fallible plan helpers and single-use results

**Files:** Create `PlanHelperCheck.{h,cpp}` and `SingleUseResultCheck.{h,cpp}`, register/build them, extend fixtures, and update affected package tests.

**Interfaces:** `fastecu-plan-helper` diagnoses helpers that fallibly build plans but unwrap/assert/throw instead of returning `Result<FlashPlan>`; `fastecu-single-use-result` handles only proven-safe adjacent declaration/assertion pairs.

- [ ] Add plan fixtures: fallible builder returning `FlashPlan` is rejected; Result propagation and forwarding an existing valid plan pass; names do not affect classification; throwing to unwrap does not evade the check.
- [ ] Add single-use fixtures: adjacent trivial Status assertion is diagnosed; intervening side effects, nontrivial owned result destruction, cleanup dependencies and multiple uses are allowed when safety is uncertain.
- [ ] Run fixtures to fail; implement conservative checks with diagnostics only. For plan classification inspect the helper body and return/data dependencies, not names.
- [ ] Convert affected helpers to return Result and assert in callers; inline only proven-safe operations. Run affected package tests.
- [ ] Enable checks only after fixtures and full-scope reports pass; commit named check/cleanup paths.

## Task 9: Containers, byte buffers and case identification

**Files:** Extend `ContainerAssertionCheck.{h,cpp}`; create `UniformBytesCheck.{h,cpp}`, `AssertionLoopTraceCheck.{h,cpp}` and `ParameterizedCaseNameCheck.{h,cpp}`; update registration, fixtures and affected tests.

**Interfaces:** Extend `fastecu-container-assertion`; register `fastecu-uniform-bytes`, `fastecu-assertion-loop-trace`, and `fastecu-parameterized-case-name`.

- [ ] Add fixtures rejecting boolean `std::equal`/ranges equality and element-checking assertion loops, while allowing ordinary `EXPECT_EQ(container, expected)` and setup-only loops. Avoid duplicate loop/container diagnostics for the same replacement.
- [ ] Add uniform-byte fixtures for constant-filled byte buffers, `Each(Eq(value))` and supported boolean algorithms; allow heterogeneous data, non-byte ranges and expressions whose uniformity cannot be established.
- [ ] Add trace fixtures for direct/invoked-lambda assertions, nested loops, traces outside the iteration and an iteration-local trace. Keep separate helper calls as an explicit uncovered case.
- [ ] Add parameterized-instantiation fixtures accepting `PrintToStringParamName` or an explicit callable name generator and rejecting omitted generators. Do not infer whether transfers are expensive or grade the names' quality.
- [ ] Run fixtures to fail, implement the four bounded checks, and clean findings with gmock matchers, `BytesFilledWith`, iteration traces or named parameterized cases as appropriate.
- [ ] Run affected package tests and full reports before enabling each check. Measure new check costs separately from migration and commit independently reviewable changes.

## Task 10: Final qualification and enduring documentation

**Files:** Update `docs/tooling.md`, `docs/coding-style.md`, documentation index and a concise qualification record under `docs/reference/`; update this plan's checkboxes from evidence only.

**Interfaces:** Delivered docs identify supported LLVM installation, platform host, one-command analysis workflow, exception schema, enabled checks and review-only limits. The coding guide owns conventions; tooling owns execution.

- [ ] Reconcile the guide with approved semantic guards, ordinary equality acceptance, exact exception mechanism and review-only expensive-outcome requirement. Generalize the existing naming-only full-cleanup statement to enabled mechanical checks.
- [ ] Run `python3 scripts/gazelle_check.py --fix` and review generated BUILD changes; run focused script/fixture tests and applicable full `bazel test --config=release //...`, `prek run --all-files`, release application build and full clang-tidy reports. Require Linux/Windows CI evidence and local macOS smoke/fixtures.
- [ ] Record full-scope zero findings, non-skipping fixtures, migration equivalence, benchmark medians and verified platform artifacts. Distinguish unexecuted platform checks from passing evidence.
- [ ] Review final diff for unrelated changes, first-party suppressions, stale custom-pass paths, exception misuse and unsupported qualification claims.
- [ ] Preserve original approved spec/plan in delivered Git history before removing completed active-work documents. Keep preservation and deletion changes separate where squash would discard originals; retain enduring knowledge in owning docs.
- [ ] Request final code review and resolve findings, then present the branch for the authorized PR workflow. Push only when authorized.

## Plan self-review

All approved coverage-table rows map to Tasks 6–9; packaging, migration, unified scope and performance map to Tasks 1–5. Task 10 owns gate evidence and enduring documentation. The Windows packaging result is intentionally an execution prerequisite: a source-build requirement returns to design review rather than being silently assumed.
