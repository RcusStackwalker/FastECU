# Sonar Dedup Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the retired NLOC hill-climbing loop with the `sonar-dedup` loop: SonarCloud nominates duplication groups, NLOC measures each step, and a CI verdict gates every loop PR.

**Architecture:** `scripts/nloc_loop.py` is renamed `scripts/dedup_loop.py` and stripped to `measure`; it gains a committed ledger, a trend log, a SonarCloud client that turns master's duplications into function-keyed groups, a freshness check, and a `verdict` command. A `sonar-dedup verdict` job in `pr.yml` runs that command after the `sonarcloud` and `mutation` jobs. A new project skill drives one iteration per firing.

**Tech Stack:** Python 3.14 stdlib only (`urllib.request`, `json`, `csv`), `unittest` under Bazel `py_test`, Lizard through pinned prek hooks, GitHub Actions, SonarCloud Web API (public, unauthenticated reads), `gh` CLI.

**Spec:** [docs/superpowers/specs/2026-10-25-sonar-dedup-loop-design.md](../specs/2026-10-25-sonar-dedup-loop-design.md) and [ADR 0029](../../adr/0029-lower-nloc-with-sonar-nominated-right-sized-abstractions.md).

## Global Constraints

- Minimum NLOC drop per loop PR: **10** (first-party NLOC, Lizard, production + tests together).
- Duplication check: PR SonarCloud `new_duplicated_lines == 0`; coverage check: PR `new_uncovered_lines == 0`.
- Mutation check: zero `Survived` mutants in the `mutation` job's reports; Mull is not run again.
- Label: `sonar-dedup` only. Every PR carrying it is a loop PR. No second marker.
- Ledger outcomes exactly: `open`, `merged`, `closed`, `failed`, `unsuitable`, `deferred`. Only `failed` counts toward the stop rule (three in a row); `unsuitable` and `deferred` neither count nor break the run.
- Ledger path `docs/reference/sonar-dedup-ledger.json` (committed). Trend path `docs/reference/dedup-trend.csv`, header `date,pr,nloc_before,nloc_after,dup_before,dup_after`.
- Stop output: a line beginning `DONE:` followed by the reason.
- SonarCloud project key `RcusStackwalker_FastECU`, host `https://sonarcloud.io`.
- Script exit codes: 0 success, 1 a rule failed, 2 tool or input failure (as today).
- Production-only nomination: test files never appear in a group (SonarCloud CPD does not analyse them).
- No new Python dependencies; no CMake; follow `docs/coding-style.md` Python rules (ruff passes).
- Work on branch `docs/sonar-dedup-loop-design`; ship as one PR to `master` after Task 6, **without** the `sonar-dedup` label: it is tooling, not a loop PR, and NLOC's scope (`src/`, `apps/`, `tests/`) does not see `scripts/`, so the verdict's NLOC rule would fail it.

## Review Focus

1. SonarCloud paging: more than one page of files with duplications (`ps` max 500) must all be read. Test in Task 3.
2. A stale analysis naming a file or lines that no longer exist at HEAD: the block is dropped with a note on stderr, never a crash. Test in Task 3.
3. A block outside any function (namespace-scope tables, includes) or spanning two functions: keyed to `<file>` or to every overlapped function, deterministically. Test in Task 3.
4. A loop PR the `mutation` job had nothing to mutate (no reports directory or empty) and a PR analysis missing a `new_*` metric: treated as 0 / pass, not a tool error. Test in Task 5.
5. SonarCloud unreachable or returning HTTP errors: exit 2 with a one-line actionable message; the verdict fails closed. Test in Tasks 3 and 5.

---

### Task 1: Settle SonarCloud's new-code period

**Files:**
- Modify: `docs/superpowers/specs/2026-10-25-sonar-dedup-loop-design.md` (Freshness section)

This is a check, not code. It decides whether Task 4 adds a master-analysis dispatch.

- [ ] **Step 1: Ask the owner to run the check** (needs their SonarCloud token; never paste it into the transcript):

```
! curl -s -u "$SONAR_TOKEN:" "https://sonarcloud.io/api/new_code_periods/show?project=RcusStackwalker_FastECU&branch=master"
```

Alternatively the owner reads Project Settings → New Code in the SonarCloud UI.

- [ ] **Step 2: Record the answer** in the spec's Freshness section, replacing the "Prerequisite" paragraph with one sentence: the setting and its consequence. `PREVIOUS_VERSION`, `REFERENCE_BRANCH` or `NUMBER_OF_DAYS` → "the loop dispatches master analyses (Task 4, variant A)". `PREVIOUS_ANALYSIS` (or unknown) → "the loop treats stale analyses as nominations only (Task 4, variant B)".

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-10-25-sonar-dedup-loop-design.md
git commit -m "docs(dedup): record SonarCloud new-code period"
```

---

### Task 2: Retire the ADR 0023 loop, keep `measure`

**Files:**
- Rename: `scripts/nloc_loop.py` → `scripts/dedup_loop.py`; `scripts/tests/nloc_loop_test.py` → `scripts/tests/dedup_loop_test.py`
- Delete: `.github/workflows/nloc-loop.yml`, `.claude/skills/hill-climbing-nloc/`, `.jscpd.json`
- Modify: `.pre-commit-config.yaml` (remove the `jscpd` hook; keep `nloc`, update its comment to point at `docs/tooling.md#sonar-dedup-loop`), `BUILD.bazel:175-185` (target `dedup_loop_test`, srcs without `mutation_test.py` until Task 5 adds it back), `.gitignore:106-107` (remove the `.nloc-loop/` entry), `bazel/coverage.bazelrc:7-8,35` (drop the nloc-loop mentions), `docs/README.md:28`, `docs/adr/0027-use-native-bazel-coverage-for-sonar.md:61-63` (say the loop's coverage guards were retired by ADR 0024), `docs/adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md` Consequences bullet (append "Amended by ADR 0029: the loop returns on SonarCloud nominations.")

**Interfaces:**
- Produces: `dedup_loop.NlocLoopError` renamed `DedupLoopError`; `parse_report(report: str) -> dict[str, int]`, `classify(path: str) -> str | None`, `Measurement`, `measure(root: Path, run_hook: RunHook = run_nloc_hook) -> Measurement`, `_run_tool(root, command) -> str`, `main(argv, *, run_hook, out, today) -> int` with only the `measure [--json]` subcommand. Later tasks add keyword-injected collaborators to `main` the same way.

- [ ] **Step 1: `git mv` both files, then delete from them** everything except `parse_report`, `classify`, `Measurement`, `measure_files`, `run_nloc_hook`, `measure`, `_run_tool`, `_parse_args` (measure only), `_run`, `main`. Keep the matching tests: `ParseReportTest`, `ClassifyTest`, `MeasureTest`, and `MainTest.test_measure_prints_the_split` / `test_measure_json`. Update the module docstring to name the new loop and `docs/tooling.md#sonar-dedup-loop`.

- [ ] **Step 2: Delete the workflow, skill, `.jscpd.json` and the `jscpd` hook; edit the references listed under Files.**

- [ ] **Step 3: Verify nothing still points at the old loop**

Run: `git grep -n -e nloc_loop -e nloc-loop -e hill-climbing -e jscpd -- ':!docs/adr/0023*' ':!docs/superpowers/specs/2026-10-1*' ':!docs/superpowers/plans/2026-10-1*' ':!docs/reference/colt-cancellation-centralization-results.md'`
Expected: only lines in this plan, the 2026-10-25 spec and ADR 0029.

- [ ] **Step 4: Run the tests and the hook**

Run: `bazel test //:dedup_loop_test && prek run --all-files && python3 scripts/dedup_loop.py measure`
Expected: test PASS; hooks pass; three lines `total N`, `production N`, `test N`.

- [ ] **Step 5: Commit**

```bash
git add -A scripts BUILD.bazel .pre-commit-config.yaml .gitignore bazel/coverage.bazelrc docs .github .claude .jscpd.json
git commit -m "refactor(tooling): retire the ADR 0023 NLOC loop, keep measure"
```

---

### Task 3: Duplication groups from SonarCloud

**Files:**
- Modify: `scripts/dedup_loop.py`, `scripts/tests/dedup_loop_test.py`, `.pre-commit-config.yaml` (new manual hook)

**Interfaces:**
- Consumes: `_run_tool`, `classify` (Task 2).
- Produces:
  - `SONAR = "https://sonarcloud.io"`, `PROJECT = "RcusStackwalker_FastECU"`
  - `Fetch = Callable[[str, Mapping[str, str]], Mapping]`: `sonar_get(path: str, params: Mapping[str, str]) -> Mapping` performs a GET on `SONAR + path`, raising `DedupLoopError("SonarCloud <path>: <reason>")` on any `URLError`/HTTP error/invalid JSON.
  - `@dataclass(frozen=True) Block: path: str; start: int; end: int` (1-based, inclusive)
  - `@dataclass(frozen=True) FunctionRange: path: str; name: str; start: int; end: int`
  - `@dataclass(frozen=True) Group: members: tuple[tuple[str, str], ...]; blocks: tuple[Block, ...]; lines: int` with property `id -> str` = first 12 hex chars of `sha256(json.dumps(members))`. `members` is the sorted, unique `(path, function)` pairs; `lines` is the sum of block sizes.
  - `duplicated_files(fetch: Fetch) -> list[str]`: paths with `duplicated_lines > 0` on master, all pages.
  - `file_groups(fetch: Fetch, path: str) -> list[tuple[Block, ...]]`: `api/duplications/show?key=PROJECT:path`.
  - `parse_function_ranges(csv_output: str) -> list[FunctionRange]` reading Lizard `--csv` rows (columns `nloc,ccn,token,param,length,location,file,function,long_name,start,end`), ignoring prek's banner lines.
  - `run_function_hook(root: Path) -> str` → `prek run nloc-functions --hook-stage manual --all-files`.
  - `key_block(block: Block, ranges: Sequence[FunctionRange]) -> list[tuple[str, str]]`: every function overlapping the block; `[(path, "<file>")]` when none.
  - `build_groups(fetch: Fetch, ranges: Sequence[FunctionRange], line_count: Callable[[str], int | None]) -> list[Group]`: dedups the same group reported from each file; drops blocks whose file is missing (`line_count` returns None) or whose `end` exceeds the file's line count, writing `dedup_loop: dropped stale block <path>:<start>-<end>` to stderr; drops a group left with fewer than two blocks; drops any block whose `classify(path)` is not production.

- [ ] **Step 1: Add the `nloc-functions` manual hook** next to `nloc` in `.pre-commit-config.yaml`: same `entry: lizard`, `language`, `additional_dependencies`, `pass_filenames`, `always_run`, `verbose`, `stages: [manual]` and exclusion args as `nloc`, with `--csv` instead of the file-table args. Run `prek run nloc-functions --hook-stage manual --all-files | head` and confirm CSV rows appear.

- [ ] **Step 2: Write failing tests** in `class SonarGroupsTest` using a fake `fetch` keyed on `(path, frozenset(params.items()))`:
  - `test_reads_every_page_of_duplicated_files`: `paging.total = 3`, `ps = 2` → returns all three paths, two requests made.
  - `test_group_reported_from_both_files_is_one_group`: A↔B duplication seen from A and from B → one `Group`, `members == (("src/a.cpp","f"),("src/b.cpp","g"))`, `lines == size_a + size_b`.
  - `test_block_outside_functions_keys_to_file`: block at lines 1-5, no range → member `("src/a.cpp","<file>")`.
  - `test_block_spanning_two_functions_keys_both`.
  - `test_stale_block_is_dropped_with_a_note`: `line_count` returns 10 for a block ending at 40 → group dropped (only one block left), stderr contains `dropped stale block src/a.cpp:30-40`.
  - `test_test_files_never_join_a_group`.
  - `test_id_is_stable_across_line_shifts`: same members, different `from` → same `id`.
  - `test_sonar_failure_is_actionable`: `sonar_get` with an opener raising `URLError("timed out")` → `DedupLoopError` whose message starts `SonarCloud api/`.
  - `class FunctionRangeTest.test_parses_lizard_csv_and_skips_banner`.

- [ ] **Step 3: Run them to verify they fail**

Run: `bazel test //:dedup_loop_test --test_output=errors`
Expected: FAIL (`AttributeError: module 'dedup_loop' has no attribute 'build_groups'`).

- [ ] **Step 4: Implement the Interfaces above in `scripts/dedup_loop.py`.** `sonar_get` uses `urllib.request.urlopen` with a 30 s timeout and `urlencode(params)`; `duplicated_files` calls `api/measures/component_tree` with `metricKeys=duplicated_lines`, `strategy=leaves`, `metricSortFilter=withMeasuresOnly`, `ps=500`, paging on `p`; `file_groups` maps the response's `_ref` to `files[ref].key` minus the `PROJECT:` prefix.

- [ ] **Step 5: Run tests to verify they pass**

Run: `bazel test //:dedup_loop_test`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add scripts .pre-commit-config.yaml
git commit -m "feat(tooling): turn SonarCloud duplications into function-keyed groups"
```

---

### Task 4: Ledger, trend, candidates and freshness

**Files:**
- Modify: `scripts/dedup_loop.py`, `scripts/tests/dedup_loop_test.py`
- Rename: `docs/reference/nloc-trend.csv` → `docs/reference/dedup-trend.csv` (rewrite: new header; old rows keep `date,pr,nloc_before,nloc_after`, empty dup columns, `kind` dropped)
- Create: `docs/reference/sonar-dedup-ledger.json` (`{"attempts": []}`)
- Create (variant A only, per Task 1): `.github/workflows/sonar-master.yml`

**Interfaces:**
- Consumes: `Group`, `build_groups`, `Fetch`, `measure` (Tasks 2-3).
- Produces:
  - `LEDGER = Path("docs/reference/sonar-dedup-ledger.json")`, `TREND = Path("docs/reference/dedup-trend.csv")`, `OUTCOMES` as in Global Constraints.
  - `@dataclass(frozen=True) Attempt: group: str; members: list[list[str]]; outcome: str; date: str; reason: str = ""; pr: int | None = None`
  - `Ledger.load(path) / save(path) / record(attempt) / outcome(group_id) -> str | None / consecutive_failures() -> int` (outcome = latest attempt for that id; consecutive failures counts trailing `failed`, skipping `unsuitable`/`deferred`, reset by `merged`/`closed`). `save` writes sorted keys, 2-space indent, trailing newline (stable diffs).
  - `TrendRow(date, pr, nloc_before, nloc_after, dup_before, dup_after: int | None)`; `append_trend(path, row)` rejects `nloc_after > nloc_before - 10` unless `pr` is a bookkeeping PR (`--bookkeeping` flag); `fill_dup_after(path, value: int)` sets the last row's empty `dup_after`.
  - `master_duplicated_lines(fetch) -> int`; `analysis_revision(fetch) -> str` (latest `api/project_analyses/search` revision).
  - `OpenPrFiles = Callable[[Path], set[str]]`: `open_non_loop_pr_files(root)` via `gh pr list --state open --json files,labels` excluding PRs labelled `sonar-dedup`.
  - `rank(groups, ledger, busy: set[str]) -> list[Group]`: drops groups whose ledger outcome is `merged`, `unsuitable`, `deferred` or `open`, and groups with any block path in `busy`; orders `src/backend/` groups first, then by `lines` descending, then `id`.
  - CLI: `candidates` (prints `"{id} lines={lines}"` then `"  {path}:{start}-{end} {function}"` per block; prints nothing when empty), `ledger record <id> <outcome> --members-json JSON --reason TEXT --pr N`, `ledger list` (ends with `consecutive failures: N`), `trend --pr N --before N --dup-before N [--bookkeeping]`, `trend fill-after`, `fresh` (exit 0 when `analysis_revision == git rev-parse origin/master`, else print both revisions and exit 1).

- [ ] **Step 1: Write failing tests**: `LedgerTest` (`test_missing_ledger_is_empty`, `test_records_survive_a_reload`, `test_later_attempt_supersedes`, `test_counts_trailing_failures`, `test_unsuitable_and_deferred_neither_count_nor_break_the_run`, `test_rejects_unknown_outcome`, `test_save_is_byte_stable`), `TrendTest` (`test_creates_header_then_appends`, `test_rejects_a_drop_under_ten`, `test_bookkeeping_row_is_exempt`, `test_fill_after_sets_only_the_last_empty_cell`), `RankTest` (`test_backend_first_then_lines`, `test_ledger_outcomes_are_skipped`, `test_groups_touching_busy_files_are_skipped`), `FreshTest` (`test_fresh_exits_zero`, `test_stale_prints_both_revisions_and_exits_one`), `MainTest.test_candidates_prints_nothing_when_empty`.

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test //:dedup_loop_test --test_output=errors`
Expected: FAIL on the missing names.

- [ ] **Step 3: Implement the Interfaces; migrate the trend CSV and create the empty ledger.**

- [ ] **Step 4 (variant A only): Add `.github/workflows/sonar-master.yml`**: `on: workflow_dispatch`, one job `uses: ./.github/workflows/sonar.yml` with `ref: master`, `cache-save: false`, the two secrets, `permissions: contents: read, pull-requests: read`. Run `python3 -m pytest tools/sonar/tests/sonar_workflow_test.py` (or its Bazel target) if it enumerates callers.

- [ ] **Step 5: Run tests to verify they pass; dry-run against live SonarCloud**

Run: `bazel test //:dedup_loop_test && python3 scripts/dedup_loop.py candidates | head -20 && python3 scripts/dedup_loop.py fresh; echo exit=$?`
Expected: PASS; the top group lists the `subaru_denso_sh7058_can*_executor.cpp` pair; `fresh` exits 1 today (master analysis is at `1b2a7287`).

- [ ] **Step 6: Commit**

```bash
git add scripts docs/reference .github
git commit -m "feat(tooling): dedup loop ledger, trend, candidates and freshness"
```

---

### Task 5: The `sonar-dedup verdict`

**Files:**
- Modify: `scripts/dedup_loop.py`, `scripts/tests/dedup_loop_test.py`, `BUILD.bazel` (add `scripts/mutation_test.py` back to `dedup_loop_test` srcs), `.github/workflows/pr.yml` (mutation job + new job)

**Interfaces:**
- Consumes: `measure`, `Fetch`, `TREND`/`LEDGER` paths; `mutation_test.read_reports(dir)`, `mutation_test.merge_reports(reports, root)`.
- Produces:
  - `pr_measures(fetch, pr: int) -> dict[str, int]` for `new_duplicated_lines`, `new_uncovered_lines`; a metric absent from the response is 0.
  - `survivors(directory: Path | None, root: Path) -> list[str]`: `"path:line:column mutator"` for each `Survived` mutant; a missing or empty directory is `[]`.
  - `is_bookkeeping(changed: Sequence[str]) -> bool`: every path is `LEDGER` or `TREND`.
  - `@dataclass(frozen=True) Check: name: str; ok: bool; detail: str`
  - `verdict(*, nloc_before: int, nloc_after: int, changed: Sequence[str], measures: Mapping[str, int], survived: Sequence[str]) -> list[Check]` with checks named exactly `nloc drop`, `new duplicated lines`, `new uncovered lines`, `surviving mutants`. `nloc drop` passes when `nloc_before - nloc_after >= 10` or `is_bookkeeping(changed)`.
  - CLI: `verdict --pr N --nloc-before N --nloc-after N --changed-file PATH --mutation-reports DIR` prints `PASS|FAIL <name>: <detail>` per check; exit 1 if any FAIL, 2 on tool failure (SonarCloud unreachable fails closed).

- [ ] **Step 1: Write failing tests** `VerdictTest`: `test_drop_of_ten_passes_nine_fails`, `test_bookkeeping_pr_skips_the_nloc_rule`, `test_any_new_duplicated_line_fails`, `test_any_new_uncovered_line_fails`, `test_absent_pr_metric_counts_as_zero`, `test_missing_report_dir_means_no_survivors`, `test_survivor_fails_and_is_named`, `MainTest.test_verdict_exits_one_on_failure`, `MainTest.test_verdict_exits_two_when_sonar_is_down`.

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test //:dedup_loop_test --test_output=errors`
Expected: FAIL on the missing names.

- [ ] **Step 3: Implement the Interfaces.**

- [ ] **Step 4: Wire CI in `.github/workflows/pr.yml`.**
  - `mutation` job: add `--report-dir mull-reports` to the `mutation_test.py` call and an `actions/upload-artifact` step (pinned by SHA like the repo's other actions; `if: always()`, `name: mutation-reports`, `path: mull-reports`, `if-no-files-found: ignore`).
  - New job `sonar-dedup-verdict`, `name: sonar-dedup verdict`, `needs: [sonarcloud, mutation]`, `if: contains(github.event.pull_request.labels.*.name, 'sonar-dedup')`, `runs-on: ubuntu-26.04`. Steps: checkout with `fetch-depth: 0`; install prek as the `pre-commit` job does; `git worktree add ../base "$(git merge-base origin/${{ github.base_ref }} HEAD)"`; `measure --json` in each (`--root ../base` for the base); `git diff --name-only <merge-base> HEAD > changed.txt`; `actions/download-artifact` for `mutation-reports` with `continue-on-error: true`; `python3 scripts/dedup_loop.py verdict ...`; append its output to `$GITHUB_STEP_SUMMARY`.
  - The `sonarcloud` job's quality-gate step already waits for SonarCloud's processing, so PR measures exist when the verdict runs.

- [ ] **Step 5: Verify**

Run: `bazel test //:dedup_loop_test //tools/sonar/tests/... && prek run --all-files` (actionlint/zizmor run inside prek if configured)
Expected: PASS; hooks pass.

- [ ] **Step 6: Commit**

```bash
git add scripts BUILD.bazel .github/workflows/pr.yml
git commit -m "feat(ci): sonar-dedup verdict gates loop PRs"
```

---

### Task 6: The `sonar-dedup-loop` skill and tooling docs

**Files:**
- Create: `.claude/skills/sonar-dedup-loop/SKILL.md`
- Modify: `docs/tooling.md:368-…` (replace the "NLOC hill-climbing loop" section with "Sonar dedup loop"), `docs/README.md:28`

**Interfaces:**
- Consumes: every CLI command from Tasks 2-5 (`measure`, `candidates`, `ledger record|list`, `trend`, `trend fill-after`, `fresh`, `verdict`).

- [ ] **Step 1: Write the skill** following the retired skill's numbered-step shape (recover it with `git show master:.claude/skills/hill-climbing-nloc/SKILL.md`). Frontmatter `name: sonar-dedup-loop`; description: one iteration of the Sonar dedup loop, used only when invoked as `/sonar-dedup-loop` or from a `/goal`/`/loop` that names it, never for ordinary refactors. Steps, each with its done-condition:
  1. **Settle the open loop PR** (`gh pr list --label sonar-dedup --state open`): checks pending → `gh pr checks <n> --watch` in the background, SonarCloud and `sonar-dedup verdict` included; green → `gh pr merge <n> --auto --squash`, record `merged`; refused → report once and hold; verdict red → fix if the refactor was wrong, else close, record `failed --reason <check>`. Stop with `DONE: three consecutive failures` when `ledger list` says 3.
  2. **Freshness** (Task 1 variant): A → after a merge, `gh workflow run sonar-master.yml`, then wait until `dedup_loop.py fresh` exits 0, then `trend fill-after`; B → proceed on the stale list. While waiting, only groups whose files the merge did not touch may be prepared.
  3. **Choose**: `candidates`; empty → `DONE: no candidates left`. Read the top group and its siblings in the same files.
  4. **Judge**: right-sized abstraction found → step 5. None, or wire evidence missing → dispatch **two** fresh Opus subagents in parallel, each given only the group's current code with enclosing functions, the five-point right-sizing test (quote it from the spec), `docs/design-notes.md` and `docs/reference/flash-contracts.md`. Both find none → `unsuitable` with both reasons; either proposes one → attempt it. No plan goldens, transcripts or scripted wire tests cover the shared path → `deferred --reason "no wire evidence"`.
  5. **Implement** on a branch `sonar-dedup/<group-id>` from fresh `master`: one abstraction, all callers, redundant family tests deleted with named owners; kill any survivors on changed lines with tests.
  6. **Right-size review**: a fresh Opus subagent gets the diff, the five-point test, the codebase-design vocabulary, `design-notes.md` and `flash-contracts.md`, not your reasoning. Any failed point → fix or abandon (`failed --reason right-size`).
  7. **Verify locally**: `measure` (drop ≥ 10), `bazel test //...`, the Linux-warning and clang-tidy checks from the repo's pre-push routine.
  8. **Open the PR** labelled `sonar-dedup`: body lists the abstraction and its domain name, the absorbed group ids, NLOC before/after, master `duplicated_lines` before, each deleted test with its owner, and the attribution footer. Commit pending ledger/trend bookkeeping on this branch. Record `open`.
  9. End with one status line, or `DONE: <reason>`; before a `DONE`, open the bookkeeping-only PR if ledger/trend changes are uncommitted.

- [ ] **Step 2: Rewrite the tooling section**: launch with `/goal run /sonar-dedup-loop iterations until it prints DONE` (fallback `/loop /sonar-dedup-loop`), the command list, the verdict's four checks, links to ADR 0029 and the spec. Update the `docs/README.md` row to "…and the Sonar dedup loop".

- [ ] **Step 3: Check the skill is listed and the old one is gone**

Run: `ls .claude/skills && git grep -n "hill-climbing-nloc" -- .claude docs/tooling.md`
Expected: `sonar-dedup-loop` present; grep prints nothing.

- [ ] **Step 4: Commit, push, open the PR**

```bash
git add .claude/skills/sonar-dedup-loop docs/tooling.md docs/README.md
git commit -m "docs(tooling): add the sonar-dedup-loop skill"
git push -u origin docs/sonar-dedup-loop-design
gh pr create --title "feat(tooling): Sonar dedup loop (ADR 0029)" --body-file <body>
```

No `sonar-dedup` label (see Global Constraints). Merge after the user approves; the loop's first real iteration starts from the merged `master`.
