# Sonar Aspect Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare Sonar C++ inputs through Bazel aspects without an application
prebuild, and transfer coverage from a separate job using a validated artifact.

**Architecture:** A package-owned aspect exports real configured compile
commands and their input closure. A Python wrapper collects declared outputs
and publishes a validated database; a separate helper exports/imports coverage
with provenance and path relocation. Qualification precedes replacement of the
existing blocking reusable workflow.

**Tech Stack:** Bazel 9.3.0, Starlark, rules_python/Python 3.14, unittest,
LLVM coverage text, existing pinned GitHub Actions.

**Spec:** [Approved design](../specs/2026-10-22-sonar-aspect-design.md).

## Global Constraints

- Use `--config=coverage` for Sonar command extraction and coverage production.
- Preserve Linux CI scope and other-language classification; support local
  macOS input preparation. Windows Sonar analysis remains separate work.
- Keep Hedron and its compatibility patch for editor integration.
- Public command: `bazel run //:sonar_inputs`; output:
  `sonar-inputs/compile_commands.json`.
- Keep scanning and its blocking Quality Gate outside cacheable Bazel actions.
- Preserve generated first-party compile entries, argument order and distinct
  owner/configuration identities. Never copy clang-tidy flag filtering.
- No application/test compilation or linking may be requested by input
  preparation. Generation tools and generated-input actions are permitted.
- Coverage handoff uses the exact resolved commit and same workflow run.
  Scanner reruns can reuse a successful earlier producer attempt.
- Retain the existing minimum of 100 resolvable first-party coverage sections.
- Python suites and test runners live in owner-local `tests/` directories.
  C++ files below are metadata fixtures, not alternate C++ test suites.
- New tooling BUILD files are hand-owned, outside the managed Gazelle roots.
  Regenerate and review any managed BUILD changes; never widen layer visibility.
- Keep the existing blocking Sonar path until qualification passes. Never
  claim cloud, Linux or performance evidence from local fixture tests.
- Preserve unrelated untracked clang-tidy documents; stage named paths only.
- Execution method: native, selected explicitly by the user. Proceed after
  plan self-review without another plan-approval round.

## Review Focus

- Parameter files and defines containing spaces must preserve the compiler's
  ordered arguments, or fail explicitly (Task 1).
- Forwarding edges and repeated configurations must retain both source
  instances instead of deduplicating by filename (Tasks 1–2).
- Remote caches and incomplete build events must not produce a plausible
  but incomplete database or leave an older successful file current (Task 2).
- A report relocated between checkout roots must alter only section headers,
  preserving source text and external paths (Task 3).
- Release-tag resolution and scanner-only reruns must select the producer's
  exact artifact and commit, independent of the current attempt (Tasks 3–4).

## File Structure

| Path | Responsibility |
| --- | --- |
| `tools/sonar/compile_commands.bzl` | Configured action extraction, forwarding, fragment and input output groups |
| `tools/sonar/sonar_commands.py` | Versioned fragment model, deterministic merge and input validation |
| `tools/sonar/sonar_inputs.py` | Bazel invocation, strict build-event collection and atomic publication |
| `tools/sonar/coverage_artifact.py` | Coverage export/import, provenance, digests and path relocation |
| `tools/sonar/sonar_qualify.py` | Baseline/candidate command comparison and evidence collection |
| `tools/sonar/BUILD.bazel` | Package binaries, libraries, tests and Sonar-scope inventory |
| `tools/sonar/tests/` | Python suites and manual configured-graph fixtures |
| `BUILD.bazel` | Public alias and existing scope-guard data dependency |
| `scripts/tests/sonar_scope_test.py` | Include new package ownership in existing scope checks |
| `.github/workflows/sonar-qualification.yml` | Temporary two-job candidate, leaving blocking workflow intact |
| `.github/workflows/sonar.yml` | Qualified final cutover preserving caller inputs and scanner check |
| `sonar-project.properties`, `.gitignore` | Final database location and ignored local outputs |
| `docs/tooling.md`, ADR 0017, qualification reference | Current operator contract, retained dependency and evidence |

### Task 1: Extract actual configured commands and generated inputs

**Files:** Create `tools/sonar/compile_commands.bzl`,
`tools/sonar/sonar_commands.py`, `tools/sonar/BUILD.bazel`,
`tools/sonar/tests/sonar_commands_test.py`,
`tools/sonar/tests/graph.bzl`, `tools/sonar/tests/graph_outputs_test.py`,
`tools/sonar/tests/probe.cpp`. Modify `BUILD.bazel` and
`scripts/tests/sonar_scope_test.py` for package inventory.

**Interfaces:**

- Aspect symbol: `sonar_compile_commands`.
- Output groups: `sonar_commands` (JSON fragments) and `sonar_inputs`
  (the referenced input closure; exclude ordinary compile/link outputs).
- Fragment schema version `1`: `owner`, `configuration`, `commands`; each
  command has `file`, ordered `arguments`, `inputs` and `outputs`. Paths remain
  execution-root-relative where Bazel supplies relative paths. Configuration
  identity uses the configured bin directory; output identity preserves
  multiple compile actions in the same owner/configuration.
- `load_fragments(paths: Sequence[Path]) -> list[dict]` validates the schema
  and nonempty compiler arguments; malformed/unsupported data raises
  `CommandError(ValueError)`.
- `database_entries(records: Sequence[dict], execroot: Path) -> list[dict]`
  produces ordered compilation-database entries with `directory`, `file`,
  `arguments`, and `output` when applicable, retaining configured instances.
- `validate_inputs(records: Sequence[dict], execroot: Path) -> None` checks
  referenced sources and declared inputs, including generated symlink targets.

- [ ] **Step 1: Add meaningful failing command-model tests.** Assert generated
  sources remain, source/owner/configuration variants remain distinct, compiler
  arguments including `-DNAME=two words` preserve order, malformed records fail,
  and unresolved `@response` arguments fail rather than disappearing.
- [ ] **Step 2: Run `bazel test --config=release //tools/sonar:sonar_commands_test`**
  and confirm failure from the absent implementation, then implement the
  schema and merge/validation functions to make those assertions pass.
- [ ] **Step 3: Add the manual real-graph regression.** An owner-local
  `graph_outputs` collector applies the aspect to a generated-source library,
  a generated header dependency, and a forwarding rule's `binary` edge under
  a copt transition. Assert two configurations of the probe, the transition
  define, generated entries and materialized headers. A deliberate compile
  failure in the probe proves the metadata target never compiles it.
- [ ] **Step 4: Run the graph test before implementing the aspect** and confirm
  missing command/input outputs. Implement extraction from `target.actions`
  for main-repository `CppCompile` actions. Traverse `deps`,
  `implementation_deps` and forwarding `binary` edges; preserve source/output
  identity and forward both output groups through non-C++ owners. Add other
  real graph edges only when parity evidence requires them.
- [ ] **Step 5: Verify actual action arguments under `compiler_param_file`.**
  Inspect the fixture's emitted arguments, not reconstructed `CcInfo` flags.
  If analysis-time arguments omit execution-time expansion, use the accurate
  `Action.args` through a declared serialization action and test that path.
  Unsupported forms fail explicitly; do not silently fall back to aquery.
- [ ] **Step 6: Wire package-owned Bazel tests and scope inventory.** Mark real
  graph fixtures/test `manual`; normal Python suites remain in `//...`.
  Add `//tools/sonar:sonar_scope_files` to the existing scope guard's data and
  register `tools/sonar` in its owner and BUILD lists. Use no new layer groups.
- [ ] **Step 7: Verify** with
  `bazel test --config=release //tools/sonar:sonar_commands_test //tools/sonar:graph_outputs_test //:sonar_scope_test`.
  Expect all suites to pass while the intentionally uncompilable fixture
  remains uncompiled. Commit named Task 1 files.

### Task 2: Add the public input-preparation command

**Files:** Create `tools/sonar/sonar_inputs.py` and
`tools/sonar/tests/sonar_inputs_test.py`; modify package/root BUILD files and
`.gitignore`. Read the existing clang-tidy BEP collector as a reference; do not
refactor that independent workflow.

**Interfaces:**

- Consume Task 1 fragments and output groups.
- `read_outputs(events: Path) -> dict[str, list[Path]]` resolves requested
  nested named sets by their BEP logical identity, not physical cache filenames.
- `prepare(workspace: Path, destination: Path, *, run: Callable) -> Path`
  builds the full `//...` graph with `--config=coverage`, only the two aspect
  output groups, reads/validates them and returns the published database path.
- `main(argv: Sequence[str] | None = None, run: Callable = subprocess.run) -> int`
  uses `BUILD_WORKSPACE_DIRECTORY`, supports `--output-dir`, and returns
  nonzero with an actionable error for unsupported hosts or failed preparation.
- Root `//:sonar_inputs` aliases the package's manual Python binary.

- [ ] **Step 1: Add failing BEP/CLI tests.** Test missing named sets, missing
  final events, unsuccessful builds with existing partial output, nonlocal
  bytestream URIs, digest-shaped cache filenames and paths with spaces. Assert
  no default compile outputs are requested and coverage configuration is used.
- [ ] **Step 2: Run `bazel test --config=release //tools/sonar:sonar_inputs_test`**
  and observe the expected missing implementation failure.
- [ ] **Step 3: Implement strict BEP collection and Bazel invocation.** Require
  successful build completion, final message, all announced events and complete
  named sets. Request only `sonar_commands,sonar_inputs`; materialize these
  requested top-level output groups even when the runner otherwise downloads
  minimally. Disable BEP URI rewriting so downloaded files have usable paths.
- [ ] **Step 4: Implement publication and validation.** Resolve the actual
  execution root, use Task 1's model, and reuse the Qt-resolution logic in
  `scripts/sonar_compdb_check.py` through an explicit package-owned dependency
  or equivalent extracted reusable function. Keep the legacy script callable.
  Remove/invalidate the old final database before starting, publish with a
  temporary sibling plus atomic replace only after every check succeeds.
- [ ] **Step 5: Add failure regression assertions.** A failed run leaves no old
  database current; a missing generated-header target fails; no fragments
  fails; local macOS uses its actual SDK; Windows receives an explicit
  unsupported-host error. Then run the Task 2 suite and scope guard to PASS.
- [ ] **Step 6: Run the real command on local macOS**, verify the ignored
  `sonar-inputs/compile_commands.json` and generated Qt inputs, and record
  requested actions separately from compiler-action metadata. Commit named
  Task 2 files. This smoke check does not qualify Linux or cloud analysis.

### Task 3: Export and import portable coverage artifacts

**Files:** Create `tools/sonar/coverage_artifact.py`,
`tools/sonar/tests/coverage_artifact_test.py`; update package BUILD targets.
Retain `scripts/coverage-local.sh` generation and uncached test execution.

**Interfaces:**

- `export_report(coverage_dir: Path, destination: Path, *, commit: str,
  run_id: str, attempt: str, producer_root: Path,
  toolchain: Mapping[str, str], configuration: str = "coverage") -> Path`
  writes the report, header manifest and `manifest.json`; returns its path.
- Schema `1` records the arguments above and SHA-256 digests of
  `llvm-cov.report` and `llvm-cov.headers`.
- `import_report(artifact_dir: Path, destination: Path, *, commit: str,
  run_id: str, consumer_root: Path, toolchain: Mapping[str, str],
  configuration: str = "coverage", min_files: int = 100) -> Path`
  verifies identity/digests, relocates only section headers and returns the
  validated report path. Errors raise `ArtifactError(ValueError)`.
- CLI subcommands `export` and `import` expose these arguments. Capture Clang,
  llvm-cov and llvm-profdata version identities from the configured installation;
  do not record credentials or unrelated environment variables.

- [ ] **Step 1: Add failing tests using temporary checkout roots.** Assert
  producer-root headers relocate while a source line containing that same
  string stays byte-identical, external headers stay unchanged, and 99
  resolvable first-party sections fail while 100 pass.
- [ ] **Step 2: Run `bazel test --config=release //tools/sonar:coverage_artifact_test`**
  and confirm the missing implementation failure; implement export/import.
- [ ] **Step 3: Add and pass identity/rerun regressions.** Wrong commit/run,
  configuration/toolchain mismatch, missing files, altered bytes and malformed
  manifests fail. A successful producer attempt remains usable on a later
  scanner attempt; a new producer gets another immutable artifact identity.
  Resolve a release-tag checkout SHA rather than trusting the event SHA.
- [ ] **Step 4: Verify the package tests and `//:sonar_scope_test`**, then commit
  named Task 3 files. Do not enable test-result caching: current raw profiles
  live outside declared test outputs and would not be restored with a hit.

### Task 4: Build a candidate workflow and qualification collector

**Files:** Create `tools/sonar/sonar_qualify.py`,
`tools/sonar/tests/sonar_qualify_test.py`,
`tools/sonar/tests/sonar_workflow_test.py`,
`.github/workflows/sonar-qualification.yml`,
`docs/reference/sonar-aspect-qualification.md`; modify package BUILD targets.
Keep `.github/workflows/sonar.yml` and production Sonar properties unchanged.

**Interfaces:**

- `compare_commands(baseline: Sequence[dict], candidate: Sequence[dict]) -> dict`
  reports missing/extra configured identities and ordered argument differences.
  Normalize execution-root prefixes and documented equivalent representations
  only; report every permitted normalization.
- Qualification CLI: `--baseline <directory> --candidate <directory>
  --output <new-directory>`. Collect full configured identities from aquery for
  the legacy baseline, because plain compilation-database JSON lacks owner
  and configuration metadata. Aquery is permitted for qualification only.
- Candidate workflow resolves the input ref in the coverage job and exposes
  `commit`, artifact name/ID and provenance through job outputs. Its scanner
  job uses `needs: coverage`, checks out that commit and imports that artifact.
  Preserve reusable `ref` and `cache-save` semantics in the candidate shape.

- [ ] **Step 1: Add failing comparison tests.** Missing generated entries,
  collapsed configurations, argument reordering or a changed define must
  produce differences; execution-root relocation alone must not. Implement
  the comparison collector and run its Bazel suite to PASS.
- [ ] **Step 2: Add workflow contract tests using the existing PyYAML dependency.**
  Assert the scanner needs coverage, checks out its resolved SHA, selects the
  producer artifact identity, validates coverage before scanning and never
  runs coverage-local.sh. Assert both producer attempts have distinct names
  and scanner-only retries do not require equal attempt numbers.
- [ ] **Step 3: Add the temporary candidate workflow.** Reuse already pinned
  checkout/setup-Bazel/upload/download/scan/Quality-Gate actions. Coverage runs
  the unchanged script, exports the artifact with `if-no-files-found: error`.
  Scanner installs its compiler/runtime prerequisites, runs `//:sonar_inputs`,
  imports coverage, and overrides only the CFamily database path for its scan.
  Keep scanner and gate failures fatal and record scan identity/logs.
- [ ] **Step 4: Add qualification evidence collection.** Record baseline and
  candidate commit, graph, compiler/LLVM versions, analyzer/profile identity,
  cache conditions, command differences, action execution and scan/coverage
  results. Mark the new reference explicitly unqualified until real evidence
  is available; do not fill results from expectations.
- [ ] **Step 5: Run package suites and scope guard**, inspect workflow diff,
  and commit named Task 4 files. The original blocking workflow remains intact.

### Task 5: Qualify and cut over the reusable workflow

**Files:** Modify `.github/workflows/sonar.yml`, `sonar-project.properties`,
`docs/tooling.md`, `docs/adr/0017-generate-bazel-targets-with-gazelle.md`,
`bazel/compile_commands/BUILD.bazel`, the qualification reference and workflow
contract test. Remove the temporary candidate workflow after qualification.

**Interfaces:** Consume Tasks 1–4 without a second command/schema. Final CI uses
two jobs inside the existing reusable workflow; caller inputs, scanner check
name and blocking Quality Gate remain stable.

- [ ] **Step 1: Establish baseline timing and budgets before candidate acceptance.**
  Record repeated cold, unchanged-warm and one-source-change input preparation
  runs, plus whole-workflow timings. State cache/server/output-base conditions
  and numerical acceptance budgets from measured baseline variability. Never
  infer a speedup solely from splitting jobs.
- [ ] **Step 2: Run full Linux qualification.** Compare all configured identities
  and normalized commands against Hedron. Use a fresh scanner output base and
  prove no application/test compile or link actions are requested, including
  cache-served actions. Verify generated sources, Qt virtual includes and
  symlink targets; distinguish required generation-tool compilation.
- [ ] **Step 3: Exercise a populated remote cache with a fresh local output base.**
  Confirm only required output groups are downloaded and available. Run actual
  scanner/Quality-Gate validation with the transferred coverage artifact.
  Compare effective source scope, findings and imported coverage against the
  baseline, explaining analyzer/profile drift. Resolve the documented C++
  test-classification discrepancy without expanding production scope.
- [ ] **Step 4: Verify macOS smoke and artifact portability/reruns.** Confirm
  the real local command succeeds without an application prebuild, a report
  moves between different checkout roots, and a failed scanner retry reuses
  the exact successful producer artifact.
- [ ] **Step 5: Record measured acceptance evidence.** If parity, no-prebuild,
  actual import/gate or performance criteria fail, keep the old blocking path
  and report the concrete issue. Missing external execution access is not
  passing evidence; finish all independent local work before requesting it.
- [ ] **Step 6: After all evidence passes, make the final cutover.** Move the
  candidate two-job shape into `sonar.yml`, set
  `sonar.cfamily.compile-commands=sonar-inputs/compile_commands.json`, update
  workflow test input and delete the temporary candidate. Preserve every other
  source/test/coverage exclusion and scanner/Quality-Gate action pin.
- [ ] **Step 7: Update owning documentation and dependency comments.** Document
  the new public/local sequence, artifact semantics and retained Hedron editor
  command in tooling; update ADR 0017's consumer explanation, not its original
  historical reasoning. Link the qualification record from tooling and the
  index. Preserve the committed spec/plan history before later cleanup.
- [ ] **Step 8: Run final gates once**, reviewing generated differences:

  ```sh
  bazel test --config=release //tools/sonar:all //:sonar_scope_test
  bazel test --config=release //tools/sonar:graph_outputs_test
  python3 scripts/gazelle_check.py --fix
  bazel build --config=release //:fastecu
  bazel test --config=release //...
  bazel run //:clang_tidy
  prek run --all-files
  ```

  Require applicable Linux/macOS/Windows platform CI and existing packaging
  gates. These are implementation-delivery gates, not commands to run while
  editing this Markdown plan.
- [ ] **Step 9: Commit named cutover and documentation files.** Obtain the
  native workflow's independent whole-branch review, address findings, and
  report actual evidence and remaining limitations. Never report migration
  complete while Task 5's qualification gates remain outstanding.

## Plan Self-Review

The five Review Focus cases map to explicit test steps. Task 1 owns configured
semantics, Task 2 owns output integrity, Task 3 owns artifact validity, Task 4
owns workflow wiring, and Task 5 owns real qualification and production cutover.
The spec's performance thresholds deliberately depend on future baseline
measurements; the plan requires recording them before accepting the candidate.
Native execution is authorized after this plan is written and checked.
