# clang-tidy Aspect Replacement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Native execution was requested by the user. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the compilation-database runner after qualifying the aspect as the sole clang-tidy gate.

**Architecture:** Keep `aspect_rules_lint` and its existing patches. Extend the Python wrapper for validated platform hosts, reliable cached reports, per-check timing and verified fixes. Keep the old runner available for qualification, then remove its exclusive interface and implementation.

**Tech Stack:** Bazel 9 (repository pin), Python 3.14, LLVM 23.1.2, aspect_rules_lint 2.9.1, Qt 6/C++23 application inputs, GoogleTest/lit fixtures.

**Spec:** [Approved design](../specs/2026-10-21-clang-tidy-replacement-design.md).

## Global Constraints

- Preserve Linux analysis, Windows-exclusive analysis and local macOS support before deleting the old runner. A macOS CI job is not required.
- Expose `bazel run //:clang_tidy` for reports and `-- --fix` for fixes; no compatibility aliases or changed-files mode in the delivered implementation.
- Keep compilation-database generation for SonarCloud and shared support required by fixtures.
- Keep the existing fuzz-harness coverage gap as separate work.
- Keep LLVM 23.1.2 and all 17 required repository checks; do not relax warning policy or check configuration.
- Fix support remains macOS/Linux; always check the final replacements and compile the selected targets with the same configuration.
- Collect timings in normal actions, including cached outputs in one descending table; `--profile` does not force re-analysis.
- Label timings as recorded wall seconds summed across translation units; do not call them CPU or current-run elapsed time.
- Use the existing feature branch, stage named paths, and preserve plans/specs in delivered history before their eventual removal.

## Review Focus

- Windows file URIs, encoded spaces and paths: reports resolve the actual local file and preserve source identity independently of cache filenames (Task 1).
- Incomplete build events and missing exit codes: a partial successful subset must never make a failed or incomplete build pass (Task 1).
- Windows target dependencies outside manifest prefixes: report and fix selection stay in the declared source scope (Task 2).
- Cached or partly absent timing data: display recorded values once and identify missing data without manufacturing zero measurements (Task 3).
- Final-pass fixes and unsuccessful compilation: no replacements escape final verification or turn a failed build into success (Task 4).

## File responsibilities

| Files | Responsibility |
| --- | --- |
| `tools/clang-tidy/scripts/clang_tidy_aspect.py`, its existing test | Commands, BEP output selection, reporting, fix orchestration |
| `tools/clang-tidy/scripts/clang_tidy_scope.py`, its existing test | Manifest validation, safe package discovery and source scope |
| `tools/clang-tidy/scripts/clang_tidy_toolchain.py`, its existing test | Shared artifact, release and check-registration validation |
| `tools/clang-tidy/scripts/clang_tidy_profile.py`, its existing test | Parse recorded check time and render one aggregate table |
| `tools/clang-tidy/aspect/BUILD.bazel`, `linters.bzl` | Platform-selected analysis executable/plugin and aspect inputs |
| `bazel/llvm.bzl`, plugin BUILD | LLVM resource inputs and narrowly scoped artifact visibility |
| Root/scripts BUILD files | Public target, tool runfiles and package-owned tests |
| `docs/reference/clang-tidy-aspect-qualification.md` | Reproducible evidence and limits of qualification |
| PR workflow and owning documentation | Sole blocking gate and current user contract after qualification |

### Task 1: Make cached output reporting fail closed

**Files:** Modify `tools/clang-tidy/scripts/clang_tidy_aspect.py`, `tools/clang-tidy/scripts/tests/clang_tidy_aspect_test.py`.

**Interfaces:** Keep `output_files(events_path: Path) -> OutputFiles`, `report(...) -> int` and `build_command(targets, events, extra) -> list[str]`. Extend `OutputFiles` with a default-empty `sources: dict[Path, str]` mapping human output to its workspace-relative source, derived from BEP output names. Add `select_outputs(files: OutputFiles, prefixes: Sequence[PurePath]) -> OutputFiles`. Preserve path lists so existing reporting tests remain useful.

- [ ] Write regression tests `test_windows_file_uri`, `test_encoded_space_in_output_uri`, `test_source_identity_uses_bep_name`, `test_missing_named_set_fails`, `test_missing_exit_code_fails`, `test_malformed_exit_code_fails`, and `test_failed_build_with_findings_remains_failure`. Assert decoded local paths, source metadata independent of URI digest, explicit `AspectError`/exit 2 for incomplete reports, and nonzero overall result after any failed Bazel build.
- [ ] Run `bazel test --config=release //tools/clang-tidy/scripts:clang_tidy_aspect_test`. Expected: the new behavior tests fail against current permissive parsing.
- [ ] Implement strict BEP traversal, required human/exit-code pairing, source metadata and source counting from that metadata. Normalize Windows drive paths without losing encoded spaces; retain existing Unix URI behavior. Reject missing referenced sets, unreadable output files, invalid exit status text and empty analysis scope. Distinguish tooling failure from findings even when both occur.
- [ ] Add `test_scope_filters_sources_not_uri_paths` with two BEP sources and digest-shaped local output filenames. Assert only the selected source's human, exit and fix outputs survive. Preserve distinct target/configuration output records for the same source.
- [ ] Implement `select_outputs`; do not identify scope from a cache URI or drop distinct configurations by filename.
- [ ] Run the task test command again. Expected: all aspect tests pass, including existing cached annotations and diagnostic deduplication.
- [ ] Commit named files as `fix: validate cached clang-tidy aspect outputs`.

### Task 2: Preserve platform hosts, scope and tool validation

**Files:** Modify `tools/clang-tidy/aspect/{BUILD.bazel,linters.bzl}`, `tools/clang-tidy/plugin/BUILD.bazel`, `bazel/llvm.bzl`, `tools/clang-tidy/scripts/{clang_tidy_aspect.py,clang_tidy_scope.py,BUILD.bazel}`, existing aspect/scope/toolchain tests, `.clang-tidy`. Preserve `scripts/clang_tidy_configs.py` and its generator gate.

**Interfaces:** Reuse `runfiles_analysis_host(environ: Mapping[str,str]) -> AnalysisHost` and `resolve_analysis_host(root: Path, artifact: Path) -> AnalysisHost`. Add `gated_packages(workspace: Path) -> list[PurePath]` and `scope_prefixes(workspace: Path, scope_os: str | None) -> tuple[PurePath,...]` in the scope module. Task 1's `select_outputs` applies that source restriction. Add wrapper option `--scope-os`; default nested builds include `--config=release`, followed by user `--bazel-arg` values.

- [ ] Add scope tests `test_package_scan_does_not_follow_bazel_or_hidden_worktrees`, `test_scope_rejects_missing_prefix`, `test_scope_rejects_uncovered_package` and wrapper tests `test_preflight_rejects_invalid_analysis_host`, `test_windows_scope_filters_dependency_outputs`, `test_release_configuration_is_default`. Assert no traversal through symlinks/junctions, explicit invalid-manifest failure, retained Windows sources only, and shared host validation before application lint actions.
- [ ] Run `bazel test --config=release //tools/clang-tidy/scripts:clang_tidy_aspect_test //tools/clang-tidy/scripts:clang_tidy_scope_test //tools/clang-tidy/scripts:clang_tidy_toolchain_test`. Expected: new behaviors fail before implementation.
- [ ] Move the legacy platform-package scan into the scope module with its symlink/junction and hidden-checkout exclusions; keep old runner working until qualification. Validate all gated packages and the selected manifest directories; narrow target patterns and output source selection for Windows. Do not add changed-file selection.
- [ ] Replace the Unix-only executable target with an alias selected by execution OS: Windows uses `//tools/clang-tidy/plugin:fastecu_clang_tidy`, other hosts use a `native_binary` wrapping `@local_llvm//:bin/clang-tidy`. Feed `plugins` a filegroup selecting an empty list on Windows and `fastecu_tidy` elsewhere; the existing patch consumes its files. Add only the required aspect-package visibility.
- [ ] Add selected artifact and `llvm_identity` runfiles to the wrapper and reuse fixture host validation. Keep fixtures working and avoid requiring LLVM for ordinary application targets. Ensure validation uses the same selected install/artifact as the aspect, rather than a separately discovered PATH executable.
- [ ] Remove `.clang-tidy-exceptions.yaml` from aspect inputs. Reconcile the root header filter to retain `src/`, `apps/`, `tests/`, `tools/` and existing `scripts/` coverage, `.h`/`.hpp` and generated exclusions. Keep the enabled checks unchanged.
- [ ] Audit executable/resource/SDK inputs. Expose selected LLVM resource headers as a filegroup in `bazel/llvm.bzl` and include it through aspect `deps`; retain executable and plugin tool inputs. Document how SDK selection enters action arguments and identify installation inputs not fully tracked. Resolve material gaps before claiming cache correctness.
- [ ] Run the task test command and `bazel run --config=release //:clang_tidy_aspect -- //src/algorithms/checksum/... --bazel-arg=--config=release`. Expected: tests pass; local macOS report loads all required checks, reports a nonempty source scope and succeeds or reports real findings without missing-file/tool errors. Correct any genuine newly exposed findings without suppressions.
- [ ] Commit named paths as `build: preserve clang-tidy platform and toolchain contracts`.

### Task 3: Aggregate one table of recorded per-check time

**Files:** Modify `tools/clang-tidy/aspect/linters.bzl`, `tools/clang-tidy/scripts/{clang_tidy_aspect.py,clang_tidy_profile.py,BUILD.bazel}`, existing aspect/profile tests.

**Interfaces:** Keep `parse_check_profile(text: str) -> dict[str,float]`. Add `render_check_table(checks: Mapping[str,float], translation_units: int, missing_profiles: int = 0) -> str`. Add wrapper `--profile`; collect tables from raw selected human outputs once, before diagnostic filtering. Keep legacy `render_markdown` until Task 6 deletes its caller.

- [ ] Write `test_one_table_includes_every_check`, `test_recorded_wall_time_label`, `test_profile_includes_cached_outputs_once`, `test_missing_profile_is_explicit` and `test_profile_does_not_change_build_command`. Use 20 distinct checks to defeat the old top-15 truncation, two cached tables with known 0.03 wall seconds and differing CPU columns, duplicate BEP references, and one missing table. Assert descending order, summed 0.06 wall seconds, explicit cache label, missing-data count and no forced-rebuild option.
- [ ] Run `bazel test --config=release //tools/clang-tidy/scripts:clang_tidy_profile_test //tools/clang-tidy/scripts:clang_tidy_aspect_test`. Expected: new table/CLI tests fail before implementation.
- [ ] Add `--enable-check-profile` to normal aspect arguments. Aggregate raw selected tables and render one check/recorded-wall-seconds/share table. Preserve parser support for optional instruction columns and malformed input protections. Do not produce the legacy phase table or compare built-ins with repository checks.
- [ ] Print the table for `--profile` and append it to `GITHUB_STEP_SUMMARY` when present, without altering lint action identity based on presentation options. Explicitly report missing timing data without treating it as measured zero.
- [ ] Run the task test command and run the same scoped aspect command twice with `--profile`. Expected: tests pass; both runs show one meaningful table, the second reuses cached actions and still reports recorded measurements.
- [ ] Commit named files as `feat: report cached clang-tidy check timings`.

### Task 4: Verify every fix and compile the selected graph

**Files:** Modify `tools/clang-tidy/scripts/clang_tidy_aspect.py`, existing aspect tests; update script BUILD dependencies if structured replacement parsing is needed.

**Interfaces:** Preserve `apply_fixes(fixes, *, workspace, tool, run) -> int`. Add `post_fix_build(targets: Sequence[str], extra: Sequence[str], *, workspace: Path, run: Run) -> None`. Reuse Task 2's effective configuration and scope. `MAX_FIX_PASSES = 3` bounds applications, followed by one mandatory final analysis if the last application consumed that budget.

- [ ] Write `test_last_fix_pass_is_rechecked`, `test_post_fix_build_uses_selected_targets_and_configuration`, `test_remaining_findings_still_compile_and_fail`, `test_post_fix_compile_failure_fails`, `test_failed_analysis_never_applies_fixes` and `test_windows_fix_fails_before_mutation`. Exercise the real orchestration with a deterministic fake subprocess boundary; assert final lint/build outcomes and file changes, rather than only mock call counts.
- [ ] Add a replacement-path regression with a checkout path containing spaces. Assert path localization preserves the replacement text and does not rewrite unrelated external files. Conflicting/unapplicable replacements must return failure.
- [ ] Run `bazel test --config=release //tools/clang-tidy/scripts:clang_tidy_aspect_test`. Expected: tests expose the current missing final check/build.
- [ ] Implement the bounded apply/check loop and final ordinary build using the same target/configuration selection. Build after successful applications even if findings remain. Stop on tooling failure; never apply fixes from an incomplete failed analysis. Keep no-op runs free of unnecessary application builds. Select `clang-apply-replacements` from the validated LLVM installation.
- [ ] Run the task test command and a disposable scoped source-edit probe with an actual fix. Restore the probe before committing. Expected: final source lints, selected targets compile, failure scenarios return nonzero and no probe changes remain.
- [ ] Commit named files as `fix: verify clang-tidy replacements and post-fix builds`.

### Task 5: Record cross-platform qualification before removal

**Files:** Create `docs/reference/clang-tidy-aspect-qualification.md`; modify `.github/workflows/pr.yml` temporarily for Linux/Windows candidate qualification while legacy targets remain available. Use existing `clang_tidy_parity.py` only until its comparison evidence is recorded.

**Interfaces:** Consume the completed candidate wrapper, existing legacy report target and parity comparator. Produce recorded evidence with exact revisions/commands/source/configuration sets and run references; Task 6 may start only after the spec's qualification requirements are met.

- [ ] Run all package tests and fixtures: `bazel test --config=release //tools/clang-tidy/scripts:all //:clang_tidy_configs_test //:sonar_scope_test`, then `bazel run --config=release //:clang_tidy_check_fixtures`. Expected: all nonmanual tests pass; real-dependency fixtures pass with pinned LLVM. Remember `:all` may include manual fixture targets explicitly; do not mistake a skipped fixture for a run.
- [ ] On local macOS, compare both full eligible source/configuration sets and findings on one revision. Seed a built-in finding, a repository-check finding and a reachable `tools/` header finding; compare diagnostic identities, then restore all seeds. Record exclusions and any reconciled differences.
- [ ] Exercise source/header/config-chain/plugin/tool identity invalidation, warm unchanged and one-source-edit timings, remote-cache output downloads and the timing table. Record which actions ran versus hit cache; record actual commands and machine/install identity, with no numerical timing gate.
- [ ] Add a temporary Linux/Windows candidate job matrix running the aspect with `--profile --annotate` and Windows scope, retaining the existing legacy gate. Qualify source/configuration sets, seeded findings, platform-exclusive code and failure cases on their respective hosts. Keep qualification scripts or probes temporary unless an enduring regression test needs them. Obtain push authorization when required; do not invent remote run results or proceed past missing platform evidence.
- [ ] Record fix-loop-limit, residual-finding and failing-build probes plus real successful fix/build evidence on macOS/Linux. Record manifest, invalid-host, empty-scope and incomplete-output rejection evidence from automated tests alongside the real-host runs.
- [ ] Review the spec's qualification table against the evidence. Expected: every row is supported; Linux and Windows candidate jobs pass and macOS evidence is current. If a requirement is unverified, keep the legacy implementation and report the exact gap.
- [ ] Run `prek run lychee --all-files` and scoped diff review. Expected: pass; no probe changes or sensitive logs are tracked.
- [ ] Commit named qualification/workflow paths as `test: qualify clang-tidy aspect replacement`.

### Task 6: Cut over the gate and delete legacy-only tooling

**Files:** Modify `BUILD.bazel`, `bazel/clang_tidy.bzl`, `bazel/compile_commands/BUILD.bazel`, `tools/clang-tidy/scripts/BUILD.bazel`, `.github/workflows/pr.yml`, `MODULE.bazel` comments, `.clang-tidy` comments, `.clang-tidy-scope.toml` comments, `AGENTS.md`, `docs/tooling.md`, `docs/adr/{0007-use-bazel-as-ci-source-of-truth.md,0017-generate-bazel-targets-with-gazelle.md}`, `docs/tech-debt.md`, `tools/clang-tidy/README.md`, documentation index. Delete `clang_tidy_runner.py`, `clang_tidy_benchmark.py`, `clang_tidy_parity.py` and their exclusive tests. Retain shared scope/toolchain/output/profile support and fixtures.

**Interfaces:** Publish root alias `//:clang_tidy` to the aspect wrapper, remove root `clang_tidy_aspect` and legacy report/fix/changed/benchmark targets. Keep `//:clang_tidy_check_fixtures` and `//bazel/compile_commands:refresh`. Keep `clang_tidy_headers` used by fixture runfiles when deleting the runner macro from `bazel/clang_tidy.bzl`.

- [ ] Confirm Task 5's qualification evidence is complete before deletion. Add/update durable smoke tests only where behavior is not already covered; do not add tests that merely mirror source text or test removal lists.
- [ ] Replace report steps inside the existing blocking Linux/Windows job names with `bazel run --config=release //:clang_tidy -- --profile --annotate` plus the matrix scope. Keep fixtures and pinned host setup; remove temporary qualification and advisory aspect jobs.
- [ ] Remove legacy targets/code/tests and unused exports/dependencies; remove the obsolete phase renderer after its last caller is deleted. Keep independent Sonar generation and fixture headers. Search with `rg -n 'clang_tidy_(runner|benchmark|parity|report|fix)|config_changed|run-clang-tidy'` and reconcile every surviving current reference; historical documents may retain clearly marked history.
- [ ] Update all owning command/contract documentation, the header-filter explanation, manifest description and ADR context. Add a tooling-guide link to the qualification evidence. Follow the writing-for-agents skill when updating `AGENTS.md`; do not change unrelated instructions.
- [ ] Regenerate managed BUILD files with `python3 scripts/gazelle_check.py --fix` and review the generated diff. Expected: package ownership and layer visibility remain valid; no guard widening.
- [ ] Run `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, `prek run --all-files`, `prek run gazelle --hook-stage pre-push --all-files`, `prek run clang-tidy-configs --hook-stage pre-push --all-files`, `bazel run --config=release //:clang_tidy -- --profile`, and fixtures. Expected: all applicable gates pass; ordinary application build still works independently of analysis installs.
- [ ] Run the replacement Linux/Windows CI jobs, confirm both are blocking and passing, and record run references. Review the whole branch with one fresh reviewer under native execution; fix important findings with regression tests and rerun affected gates.
- [ ] Preserve this plan, approved design and spike contents in delivered Git history before removing completed plans/specs. Extract enduring knowledge first; keep history-preservation and deletion changes separate if squash delivery would discard original contents. Update the index to link to current tooling/evidence, then run Markdown gates and scoped diff review.
- [ ] Commit named final paths as `build: replace legacy clang-tidy runner with Bazel aspect`. Deliver through a PR; do not merge or push to a shared branch without authorization.

## Self-review and execution

All spec sections map to Tasks 1–6. Task 1 owns output identity and failure
handling; Task 2 consumes that identity for platform scope; Tasks 3 and 4 reuse
the same selected outputs/configuration. Task 5 qualifies their combined
behavior before Task 6 deletes the comparison baseline. The Review Focus cases
are assigned to the owning test task above.

The user requested native execution immediately after planning. Continue with
`superpowers:executing-plans`; do not ask again for the execution method.
