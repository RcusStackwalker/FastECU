# clang-tidy CI speed-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Take clang-tidy off the PR critical path and shrink it: profile it, run only Windows-exclusive code on Windows, and run it as its own parallel job.

**Architecture:** The runner (`scripts/clang_tidy_runner.py`) gains phase timers and a `--profile` flag (Part 1), then a `--scope-os` flag backed by a checked-in scope manifest and a coverage guard (Part 2). `pr.yml` then moves tidy out of the `bazel` job into a Linux + Windows `clang-tidy` job (Part 3). Each part is one stacked PR.

**Tech Stack:** Python 3.14 (`unittest`, `tomllib`), Bazel `py_test`/`py_binary`, GitHub Actions, LLVM `run-clang-tidy`.

**Spec:** [clang-tidy CI speed-up design](../specs/2026-10-03-clang-tidy-ci-speedup-design.md) (amended by Task 1 of this plan).

## Global Constraints

- Git: work lands through PRs; branch, commit, push only when authorized; `prek` refuses commits on `master`. Stack the three PRs with `gh stack` (`gh stack init`, then `gh stack submit --auto`).
- Commit messages end with the two attribution lines from the session reminder (`Co-Authored-By: ...`, `Claude-Session: ...`).
- Python tests are `unittest`, run through Bazel: `bazel test --config=release //:clang_tidy_runner_test //:clang_tidy_profile_test //:clang_tidy_scope_test`.
- Warnings are errors in first-party code; no `#pragma`/`-Wno-*` opt-outs (not expected here, Python/YAML only).
- Workflow actions are pinned by commit SHA exactly as the existing `pr.yml` pins them; copy the pins, do not invent new ones.
- `.clang-tidy` checks are not changed.
- Ratchet: the `qt_layer` package group in `bazel/qt/BUILD.bazel` is not touched.
- Cross-document references in Markdown are links with readable text, not backticked paths (lychee).

## Review Focus

- `run-clang-tidy` prints the profile report as one aggregated table in LLVM 23 but may print one per file on older LLVM (Ubuntu apt): the parser must sum rows by check name across any number of tables (Task 2 test).
- A tidy run with `--profile` that finds zero rows (flag unsupported, output truncated) must not fail the gate: render an explicit "no profile data" note (Task 2 test).
- `GITHUB_STEP_SUMMARY` unwritable must warn, never fail the gate (Task 3 test).
- A new platform-gated package must fail the guard until the manifest names it, and a gated `BUILD.bazel` with no C/C++ sources (`bazel/platforms`) must not (Task 5 test).
- `--scope-os` with an OS the manifest lacks must fail with a clear error, and a manifest path that is absolute or contains `..` must be rejected (Task 5 test).
- With `--scope-os windows --changed`, a changed header outside the Windows prefixes yields no work and exits cleanly, not an error (Task 6 test).

---

## PART 1 — Profiling (PR 1)

### Task 1: Branch the stack and amend the spec

The audit while planning found two facts that change the spec: there is **no macOS-exclusive code** (every `target_compatible_with` is Windows-only or "not Windows", and the latter is fully analyzed on Linux), and `run-clang-tidy` has no `-store-check-profile` passthrough, only an aggregated `-enable-check-profile` report (no per-file attribution).

**Files:**
- Modify: `docs/superpowers/specs/2026-10-03-clang-tidy-ci-speedup-design.md`

- [ ] **Step 1: Cut the first stack branch from the spec branch**

```bash
git checkout docs/clang-tidy-ci-speedup-design
gh stack init
```

Expected: a stack whose first layer is `docs/clang-tidy-ci-speedup-design`; add a layer for Part 1 (`gh stack add ci/tidy-profiling`). If `gh stack` prompts, follow the memory note on stacked PRs (init + submit --auto).

- [ ] **Step 2: Amend the spec**

In Part 1 of the spec replace the bullets "`--profile-dir` flag", "Aggregation" and the artifact sentence with:

```markdown
- **`--profile` flag.** Adds `-enable-check-profile` to the `run-clang-tidy`
  command. `run-clang-tidy` has no `-store-check-profile` passthrough, so there
  is no per-file JSON; the aggregated per-check report is what we get.
- **Aggregation.** The runner parses that report (summing wall time per check
  across however many tables the LLVM version prints) and writes a markdown
  summary to `$GITHUB_STEP_SUMMARY`: top 15 checks by total time and the phase
  timings. Per-translation-unit ranking is out of scope.
```

In Part 2 replace the "Scope manifest" and "Per-OS behavior" bullets with:

```markdown
- **Scope manifest** (`.clang-tidy-scope.toml`). Lists the path prefixes of
  Windows-exclusive code, and the platform-gated packages that the full Linux
  run already covers. An audit of every `target_compatible_with` found no
  macOS-exclusive code, so **there is no macOS tidy job**: Linux covers
  everything except Windows-exclusive code.
- **Per-OS behavior.** Linux analyzes everything in its compile DB. Windows
  analyzes only translation units under the manifest's Windows prefixes.
```

In Part 3 change "matrix over the three OSes" to "matrix over Linux and Windows", and in "Accepted trade-off" append: "macOS loses tidy entirely; two files with `__APPLE__` guards (`qt_calibration_interaction_test.cpp`, `mock_openport.h`) are no longer analyzed under macOS defines."

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-10-03-clang-tidy-ci-speedup-design.md
git commit -m "docs: amend clang-tidy speed-up design after audit (no macOS job, aggregated profile)"
```

(Append the two attribution lines to the message.)

### Task 2: Profile report parser

**Files:**
- Create: `scripts/clang_tidy_profile.py`
- Create: `scripts/clang_tidy_profile_test.py`
- Modify: `BUILD.bazel` (add `py_test`, next to `clang_tidy_runner_test` at ~line 127)

**Interfaces:**
- Produces: `parse_check_profile(text: str) -> dict[str, float]`; `render_markdown(checks: Mapping[str, float], phases: Mapping[str, float], translation_units: int, top: int = 15) -> str`.

- [ ] **Step 1: Write the failing tests**

`scripts/clang_tidy_profile_test.py`:

```python
#!/usr/bin/env python3

import unittest

import clang_tidy_profile as profile

_TABLE = """\
===-------------------------------------------------------------------------===
                          clang-tidy checks profiling
===-------------------------------------------------------------------------===
  Total Execution Time: 0.0500 seconds (0.0600 wall clock)

   ---User Time---   --System Time--   --User+System--   ---Wall Time---  --- Name ---
   0.0300 (60.0%)   0.0100 (50.0%)   0.0400 (57.1%)   0.0300 (50.0%)  bugprone-infinite-loop
   0.0200 (40.0%)   0.0100 (50.0%)   0.0300 (42.9%)   0.0300 (50.0%)  readability-braces-around-statements
   0.0500 (100.0%)   0.0200 (100.0%)   0.0700 (100.0%)   0.0600 (100.0%)  Total
"""

_TABLE_WITH_INSTR = """\
   ---User Time---   --System Time--   --User+System--   ---Wall Time---  ---Instr---  --- Name ---
   0.0100 (50.0%)   0.0000 (0.0%)   0.0100 (50.0%)   0.0100 (50.0%)   20723 (50.0%)  bugprone-infinite-loop
   0.0100 (50.0%)   0.0000 (0.0%)   0.0100 (50.0%)   0.0100 (50.0%)   20723 (50.0%)  Total
"""


class ParseCheckProfileTest(unittest.TestCase):
    def test_reads_wall_time_per_check_and_skips_total(self) -> None:
        checks = profile.parse_check_profile(_TABLE)
        self.assertEqual(
            {
                "bugprone-infinite-loop": 0.03,
                "readability-braces-around-statements": 0.03,
            },
            checks,
        )

    def test_sums_the_same_check_across_several_tables(self) -> None:
        checks = profile.parse_check_profile(_TABLE + "other output\n" + _TABLE)
        self.assertAlmostEqual(0.06, checks["bugprone-infinite-loop"])

    def test_tolerates_an_instruction_count_column(self) -> None:
        checks = profile.parse_check_profile(_TABLE_WITH_INSTR)
        self.assertEqual({"bugprone-infinite-loop": 0.01}, checks)

    def test_ignores_text_without_a_table(self) -> None:
        self.assertEqual({}, profile.parse_check_profile("warning: something\n"))


class RenderMarkdownTest(unittest.TestCase):
    def test_lists_phases_and_checks_slowest_first(self) -> None:
        text = profile.render_markdown(
            {"a-fast": 1.0, "b-slow": 9.0},
            {"prebuild": 12.34, "analysis": 5.0},
            translation_units=42,
        )
        self.assertIn("Translation units: 42", text)
        self.assertIn("| prebuild | 12.3 |", text)
        self.assertLess(text.index("b-slow"), text.index("a-fast"))
        self.assertIn("| b-slow | 9.0 | 90.0% |", text)

    def test_limits_the_check_table_to_top(self) -> None:
        checks = {f"check-{index}": float(index + 1) for index in range(5)}
        text = profile.render_markdown(checks, {}, translation_units=1, top=2)
        self.assertIn("check-4", text)
        self.assertIn("check-3", text)
        self.assertNotIn("check-2", text)

    def test_says_so_when_there_is_no_profile_data(self) -> None:
        text = profile.render_markdown({}, {"analysis": 1.0}, translation_units=3)
        self.assertIn("No check profile data", text)
        self.assertIn("| analysis | 1.0 |", text)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Add the Bazel test target**

In `BUILD.bazel`, directly after the `clang_tidy_runner_test` `py_test`:

```starlark
py_test(
    name = "clang_tidy_profile_test",
    size = "small",
    srcs = [
        "scripts/clang_tidy_profile.py",
        "scripts/clang_tidy_profile_test.py",
    ],
    imports = ["scripts"],
    main = "scripts/clang_tidy_profile_test.py",
)
```

Also add `"scripts/clang_tidy_profile.py",` to the `srcs` of `clang_tidy_runner_test` (the runner will import it in Task 3).

- [ ] **Step 3: Run to verify failure**

Run: `bazel test --config=release //:clang_tidy_profile_test`
Expected: FAIL (`ModuleNotFoundError: clang_tidy_profile`, or missing source file).

- [ ] **Step 4: Implement**

`scripts/clang_tidy_profile.py`:

```python
"""Summarize the report `run-clang-tidy -enable-check-profile` prints."""

from __future__ import annotations

import re
from collections.abc import Mapping

# A table row is N columns of `<seconds> (<percent>%)` followed by the check
# name. The columns are user, system, user+system, wall and, on newer LLVM,
# an instruction count; wall time is always the fourth.
_COLUMN = re.compile(r"(\d+(?:\.\d+)?(?:e[+-]?\d+)?)\s+\(\s*\d+(?:\.\d+)?%\)")
_WALL_COLUMN = 3


def parse_check_profile(text: str) -> dict[str, float]:
    """Wall seconds per check, summed over every table found in `text`.

    run-clang-tidy aggregates one table on recent LLVM but may print one per
    file on older releases; summing by name handles both. The `Total` row is
    dropped.
    """
    totals: dict[str, float] = {}
    for line in text.splitlines():
        columns = _COLUMN.findall(line)
        if len(columns) <= _WALL_COLUMN:
            continue
        name = line.rsplit(None, 1)[-1]
        if name == "Total" or name.endswith(")"):
            continue
        totals[name] = totals.get(name, 0.0) + float(columns[_WALL_COLUMN])
    return totals


def render_markdown(
    checks: Mapping[str, float],
    phases: Mapping[str, float],
    translation_units: int,
    top: int = 15,
) -> str:
    lines = [
        "### clang-tidy profile",
        "",
        f"Translation units: {translation_units}",
        "",
        "| Phase | Seconds |",
        "|---|---:|",
    ]
    lines.extend(f"| {name} | {seconds:.1f} |" for name, seconds in phases.items())
    lines.append("")
    if not checks:
        lines.append("No check profile data was found in the clang-tidy output.")
        lines.append("")
        return "\n".join(lines)
    total = sum(checks.values())
    lines.extend(
        [
            "| Check | Seconds (summed over files) | Share |",
            "|---|---:|---:|",
        ]
    )
    slowest = sorted(checks.items(), key=lambda item: item[1], reverse=True)[:top]
    for name, seconds in slowest:
        share = 100.0 * seconds / total if total else 0.0
        lines.append(f"| {name} | {seconds:.1f} | {share:.1f}% |")
    lines.append("")
    return "\n".join(lines)
```

- [ ] **Step 5: Run to verify pass**

Run: `bazel test --config=release //:clang_tidy_profile_test`
Expected: PASS (7 tests).

- [ ] **Step 6: Commit**

```bash
git add scripts/clang_tidy_profile.py scripts/clang_tidy_profile_test.py BUILD.bazel
git commit -m "feat: parse clang-tidy check-profile reports"
```

### Task 3: Phase timers and the `--profile` flag in the runner

**Files:**
- Modify: `scripts/clang_tidy_runner.py` (imports; new `PhaseTimings`, `_emit_profile`; `run_workflow`; `main`)
- Modify: `scripts/clang_tidy_runner_test.py`
- Modify: `bazel/clang_tidy.bzl` (srcs, imports)

**Interfaces:**
- Consumes: `clang_tidy_profile.parse_check_profile`, `clang_tidy_profile.render_markdown` (Task 2).
- Produces: `PhaseTimings(clock=time.monotonic)` with `.durations: dict[str, float]` and `.phase(name)` context manager that prints `clang-tidy: <name> took <s>s`; `run_workflow(..., profile: bool = False)`; CLI `--profile`.

- [ ] **Step 1: Write the failing tests**

Add to `scripts/clang_tidy_runner_test.py` (new class at the end of the file, before the `if __name__` block; reuse the module-level helpers):

```python
class PhaseTimingsTest(unittest.TestCase):
    def test_records_and_prints_each_phase(self) -> None:
        ticks = iter([10.0, 12.5])
        timings = runner.PhaseTimings(clock=lambda: next(ticks))
        output = StringIO()
        with redirect_stdout(output), timings.phase("prebuild"):
            pass
        self.assertEqual({"prebuild": 2.5}, timings.durations)
        self.assertIn("clang-tidy: prebuild took 2.5s", output.getvalue())

    def test_records_a_phase_that_raises(self) -> None:
        ticks = iter([0.0, 1.0])
        timings = runner.PhaseTimings(clock=lambda: next(ticks))
        with redirect_stdout(StringIO()), self.assertRaises(RuntimeError):
            with timings.phase("analysis"):
                raise RuntimeError("boom")
        self.assertEqual({"analysis": 1.0}, timings.durations)
```

and inside `ClangTidyRunnerTest` (after `test_report_does_not_turn_compiler_warnings_into_errors`):

```python
    def run_with_report(
        self, *, profile: bool, report: str, environ: dict[str, str]
    ) -> tuple[list[list[str]], str]:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        commands: list[list[str]] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            if "-clang-tidy-binary" in command:
                return subprocess.CompletedProcess(command, 0, stdout=report)
            return subprocess.CompletedProcess(command, 0)

        output = StringIO()
        with (
            mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS),
            redirect_stdout(output),
        ):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="linux",
                environ=environ,
                command_runner=fake_run,
                profile=profile,
            )
        return commands, output.getvalue()

    _REPORT = (
        "   0.0300 (100.0%)   0.0000 (0.0%)   0.0300 (100.0%)   0.0300 (100.0%)  bugprone-infinite-loop\n"
    )

    def test_profile_flag_enables_the_check_profile_and_prints_a_summary(self) -> None:
        commands, output = self.run_with_report(profile=True, report=self._REPORT, environ={})
        self.assertIn("-enable-check-profile", commands[-1])
        self.assertIn("### clang-tidy profile", output)
        self.assertIn("bugprone-infinite-loop", output)
        self.assertIn("clang-tidy: analysis took", output)

    def test_no_profile_flag_leaves_the_command_unchanged(self) -> None:
        commands, output = self.run_with_report(profile=False, report=self._REPORT, environ={})
        self.assertNotIn("-enable-check-profile", commands[-1])
        self.assertNotIn("### clang-tidy profile", output)

    def test_profile_summary_goes_to_github_step_summary(self) -> None:
        summary = self.root / "summary.md"
        _, output = self.run_with_report(
            profile=True, report=self._REPORT, environ={"GITHUB_STEP_SUMMARY": str(summary)}
        )
        self.assertIn("bugprone-infinite-loop", summary.read_text())
        self.assertNotIn("### clang-tidy profile", output)

    def test_unwritable_step_summary_warns_instead_of_failing(self) -> None:
        missing = self.root / "no-such-dir" / "summary.md"
        stderr = StringIO()
        with mock.patch.object(runner.sys, "stderr", stderr):
            self.run_with_report(
                profile=True, report=self._REPORT, environ={"GITHUB_STEP_SUMMARY": str(missing)}
            )
        self.assertIn("could not write profile summary", stderr.getvalue())
```

- [ ] **Step 2: Run to verify failure**

Run: `bazel test --config=release //:clang_tidy_runner_test`
Expected: FAIL (`PhaseTimings` / `profile` argument do not exist).

- [ ] **Step 3: Implement in `scripts/clang_tidy_runner.py`**

Imports: add `import time`, `from collections.abc import Iterator`, `from contextlib import contextmanager`, and `import clang_tidy_profile as profile_report` (after `import yaml`; keep the file's import ordering style, `ruff` will tell you).

Add after `CommandRunner = ...`:

```python
class PhaseTimings:
    """Wall-clock duration of each named phase, printed as each one ends."""

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self._clock = clock
        self.durations: dict[str, float] = {}

    @contextmanager
    def phase(self, name: str) -> Iterator[None]:
        start = self._clock()
        try:
            yield
        finally:
            elapsed = self._clock() - start
            self.durations[name] = self.durations.get(name, 0.0) + elapsed
            print(f"clang-tidy: {name} took {elapsed:.1f}s")


def _emit_profile(
    report: str,
    timings: PhaseTimings,
    translation_units: int,
    environ: Mapping[str, str],
) -> None:
    """Publish the profile summary; a failure to write it never fails the gate."""
    markdown = profile_report.render_markdown(
        profile_report.parse_check_profile(report),
        timings.durations,
        translation_units,
    )
    summary_path = environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        print(markdown)
        return
    try:
        with open(summary_path, "a", encoding="utf-8") as stream:
            stream.write(markdown)
    except OSError as error:
        print(f"clang-tidy: warning: could not write profile summary: {error}", file=sys.stderr)
```

In `run_workflow`: add the keyword `profile: bool = False` after `changed: bool = False`. Replace the region from `_prebuild(command_runner, build_args, workspace)` through the `if changed:` block with:

```python
    timings = PhaseTimings()
    with timings.phase("prebuild"):
        _prebuild(command_runner, build_args, workspace)
    with timings.phase("compdb-refresh"):
        refresh_code = _run(command_runner, [compdb_tool, *compdb_args], workspace)
    if refresh_code:
        raise WorkflowError(f"compilation database refresher failed with exit code {refresh_code}")

    with timings.phase("filter"):
        entries = load_project_entries(workspace, workspace / "compile_commands.json")
        if changed:
            changed_paths = changed_files(workspace, command_runner)
            entries, notes = filter_changed_entries(entries, changed_paths, workspace.resolve())
            for note in notes:
                print(note)
    if changed and not entries:
        print("clang-tidy: no changed C/C++ translation units to analyze, skipping.")
        return
```

Before `print(f"Analyzing {len(entries)} translation units ...")` add:

```python
        if profile:
            command.append("-enable-check-profile")
```

Replace `tidy_result = _run_quiet(command_runner, command, workspace)` with:

```python
        with timings.phase("analysis"):
            tidy_result = _run_quiet(command_runner, command, workspace)
        if profile:
            _emit_profile(tidy_result.stdout or "", timings, len(entries), environ)
```

In `main`: add `parser.add_argument("--profile", action="store_true")` and pass `profile=args.profile` to `run_workflow`.

- [ ] **Step 4: Make the Bazel binary see the new module**

In `bazel/clang_tidy.bzl`, change the `py_binary` call:

```starlark
        srcs = [
            "//:scripts/clang_tidy_profile.py",
            "//:scripts/clang_tidy_runner.py",
        ],
        imports = ["scripts"],
```

If Bazel reports the `//:scripts/...` labels are not visible, add them to the root `exports_files(...)` list next to `".clang-tidy"` in `BUILD.bazel`.

- [ ] **Step 5: Run to verify pass**

Run: `bazel test --config=release //:clang_tidy_runner_test //:clang_tidy_profile_test`
Expected: PASS. Then a smoke run of the real tool on this machine:
`bazel run --config=release //:clang_tidy_report_changed -- --profile`
Expected: `clang-tidy: prebuild took …s` lines, and either a profile table or "no changed C/C++ translation units" (then the profile is not emitted; acceptable).

- [ ] **Step 6: Commit**

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py bazel/clang_tidy.bzl BUILD.bazel
git commit -m "feat: clang-tidy runner phase timers and --profile summary"
```

### Task 4: Turn profiling on in CI and document it

**Files:**
- Modify: `.github/workflows/pr.yml` (the `clang-tidy report` step at the end of the `bazel` job)
- Modify: `docs/coding-style.md` (the `## Static analysis` list, ~line 421)

- [ ] **Step 1: Edit the workflow step**

```yaml
      - name: clang-tidy report
        run: |
          bazel run --config=release //:clang_tidy_report_changed -- --profile
```

- [ ] **Step 2: Document the flag**

In `docs/coding-style.md`, append to the `bazel run //:clang_tidy_report_changed` bullet: ``Add `-- --profile` to see phase timings and the slowest checks.``

- [ ] **Step 3: Verify and commit**

Run: `prek run --all-files`
Expected: all hooks pass (including the link check).

```bash
git add .github/workflows/pr.yml docs/coding-style.md
git commit -m "ci: record clang-tidy profile on every PR run"
```

- [ ] **Step 4: Submit slice 1**

`gh stack submit --auto` only when the user authorizes pushing. After it merges, read the profile on a few PR runs (Actions run summary) before starting Part 2; it should confirm the share held by Linux versus Windows analysis and the prebuild phase. If prebuild dominates, note it for Part 3's design risk.

---

## PART 2 — Scope manifest and `--scope-os` (PR 2)

### Task 5: Scope module and manifest

**Files:**
- Create: `.clang-tidy-scope.toml`
- Create: `scripts/clang_tidy_scope.py`
- Create: `scripts/clang_tidy_scope_test.py`
- Modify: `BUILD.bazel` (py_test; `clang_tidy_runner_test` srcs)

**Interfaces:**
- Produces:
  - `ScopeManifest(os_prefixes: Mapping[str, tuple[PurePath, ...]], linux_covered: tuple[PurePath, ...])`
  - `load_manifest(path: Path) -> ScopeManifest` (raises `ScopeError`)
  - `in_scope(relative: PurePath, prefixes: Iterable[PurePath]) -> bool`
  - `build_targets(prefixes: Iterable[PurePath]) -> list[str]`
  - `uncovered_gated_packages(packages: Iterable[PurePath], manifest: ScopeManifest) -> list[PurePath]`
  - `class ScopeError(ValueError)`

- [ ] **Step 1: Create the manifest**

`.clang-tidy-scope.toml`:

```toml
# Which OS runs clang-tidy over platform-gated code. Linux's job analyzes every
# translation unit in its own compile database; other OSes analyze only their
# exclusive code. Prefixes are workspace-relative directories, forward slashes.
#
# Every BUILD.bazel that sets target_compatible_with and has C/C++ sources must
# fall under a prefix below, or the runner's guard fails. When it does, decide:
# exclusive to one OS -> [os.<name>]; otherwise already built (and so analyzed)
# on Linux -> [linux_covered].

[os.windows]
prefixes = [
  "src/platform/desktop/windows",
  "src/platform/desktop/common/serial",
  "tests",
]

[linux_covered]
prefixes = [
  "src/platform/desktop/unix/j2534",
  "src/platform/desktop/common/serial/direct/unix",
]
```

Note `common/serial` and `tests` are whole-package prefixes because each mixes Windows-only and portable targets; analyzing a few extra portable files on Windows is a harmless superset.

- [ ] **Step 2: Write the failing tests**

`scripts/clang_tidy_scope_test.py`:

```python
#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path, PurePath

import clang_tidy_scope as scope


def _manifest() -> scope.ScopeManifest:
    return scope.ScopeManifest(
        os_prefixes={"windows": (PurePath("src/win"), PurePath("tests"))},
        linux_covered=(PurePath("src/unix"),),
    )


class LoadManifestTest(unittest.TestCase):
    def write(self, text: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / scope.MANIFEST_NAME
        path.write_text(text)
        return path

    def test_loads_prefixes(self) -> None:
        manifest = scope.load_manifest(
            self.write(
                '[os.windows]\nprefixes = ["src/win", "tests"]\n'
                '[linux_covered]\nprefixes = ["src/unix"]\n'
            )
        )
        self.assertEqual((PurePath("src/win"), PurePath("tests")), manifest.os_prefixes["windows"])
        self.assertEqual((PurePath("src/unix"),), manifest.linux_covered)

    def test_rejects_absolute_and_parent_relative_prefixes(self) -> None:
        for bad in ('"/etc"', '"../outside"', '"src/../x"', '"C:/x"'):
            with self.subTest(bad=bad), self.assertRaises(scope.ScopeError):
                scope.load_manifest(self.write(f"[os.windows]\nprefixes = [{bad}]\n"))

    def test_rejects_a_missing_file_and_bad_toml(self) -> None:
        with self.assertRaises(scope.ScopeError):
            scope.load_manifest(Path("/no/such/.clang-tidy-scope.toml"))
        with self.assertRaises(scope.ScopeError):
            scope.load_manifest(self.write("[os.windows\n"))

    def test_rejects_non_string_prefixes(self) -> None:
        with self.assertRaises(scope.ScopeError):
            scope.load_manifest(self.write("[os.windows]\nprefixes = [1]\n"))


class ScopeQueriesTest(unittest.TestCase):
    def test_in_scope_matches_a_directory_and_its_descendants_only(self) -> None:
        prefixes = [PurePath("src/win")]
        self.assertTrue(scope.in_scope(PurePath("src/win/a.cpp"), prefixes))
        self.assertTrue(scope.in_scope(PurePath("src/win/deep/b.cpp"), prefixes))
        self.assertFalse(scope.in_scope(PurePath("src/windows_like/a.cpp"), prefixes))
        self.assertFalse(scope.in_scope(PurePath("src/a.cpp"), prefixes))

    def test_build_targets_are_recursive_patterns(self) -> None:
        self.assertEqual(
            ["//src/win/...", "//tests/..."],
            scope.build_targets([PurePath("src/win"), PurePath("tests")]),
        )

    def test_guard_reports_packages_no_prefix_covers(self) -> None:
        packages = [
            PurePath("src/win/j2534"),
            PurePath("src/unix"),
            PurePath("tests/force_asserts"),
            PurePath("src/new_gated"),
        ]
        self.assertEqual(
            [PurePath("src/new_gated")], scope.uncovered_gated_packages(packages, _manifest())
        )

    def test_guard_accepts_when_everything_is_covered(self) -> None:
        self.assertEqual(
            [], scope.uncovered_gated_packages([PurePath("src/win")], _manifest())
        )


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 3: Add the Bazel test target**

In `BUILD.bazel`, after `clang_tidy_profile_test`:

```starlark
py_test(
    name = "clang_tidy_scope_test",
    size = "small",
    srcs = [
        "scripts/clang_tidy_scope.py",
        "scripts/clang_tidy_scope_test.py",
    ],
    imports = ["scripts"],
    main = "scripts/clang_tidy_scope_test.py",
)
```

Add `"scripts/clang_tidy_scope.py",` to `clang_tidy_runner_test` `srcs`.

- [ ] **Step 4: Run to verify failure**

Run: `bazel test --config=release //:clang_tidy_scope_test`
Expected: FAIL (`ModuleNotFoundError: clang_tidy_scope`).

- [ ] **Step 5: Implement**

`scripts/clang_tidy_scope.py`:

```python
"""Per-OS scope manifest for the clang-tidy runner."""

from __future__ import annotations

import tomllib
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from itertools import chain
from pathlib import Path, PurePath, PurePosixPath, PureWindowsPath

MANIFEST_NAME = ".clang-tidy-scope.toml"


class ScopeError(ValueError):
    """The scope manifest is missing, malformed, or names an unsafe path."""


@dataclass(frozen=True)
class ScopeManifest:
    """Which code each OS's tidy run owns.

    `os_prefixes` maps an OS to the directories only it builds; `linux_covered`
    lists platform-gated directories the full Linux run already analyzes.
    """

    os_prefixes: Mapping[str, tuple[PurePath, ...]]
    linux_covered: tuple[PurePath, ...]


def _prefixes(value: object, where: str) -> tuple[PurePath, ...]:
    if not isinstance(value, list):
        raise ScopeError(f"{where}: prefixes must be a list")
    result: list[PurePath] = []
    for item in value:
        if not isinstance(item, str) or not item:
            raise ScopeError(f"{where}: prefix {item!r} is not a non-empty string")
        posix = PurePosixPath(item)
        if (
            posix.is_absolute()
            or PureWindowsPath(item).is_absolute()
            or PureWindowsPath(item).drive
            or ".." in posix.parts
        ):
            raise ScopeError(f"{where}: prefix {item!r} must be relative and stay in the workspace")
        result.append(PurePath(*posix.parts))
    return tuple(result)


def load_manifest(path: Path) -> ScopeManifest:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as error:
        raise ScopeError(f"cannot read scope manifest {path}: {error}") from error
    operating_systems = data.get("os", {})
    if not isinstance(operating_systems, dict):
        raise ScopeError("[os] must be a table")
    os_prefixes = {
        name: _prefixes(table.get("prefixes", []), f"[os.{name}]")
        for name, table in operating_systems.items()
        if isinstance(table, dict)
    }
    covered = data.get("linux_covered", {})
    if not isinstance(covered, dict):
        raise ScopeError("[linux_covered] must be a table")
    return ScopeManifest(
        os_prefixes=os_prefixes,
        linux_covered=_prefixes(covered.get("prefixes", []), "[linux_covered]"),
    )


def in_scope(relative: PurePath, prefixes: Iterable[PurePath]) -> bool:
    """True when `relative` is one of `prefixes` or lives beneath one."""
    return any(prefix == relative or prefix in relative.parents for prefix in prefixes)


def build_targets(prefixes: Iterable[PurePath]) -> list[str]:
    return [f"//{prefix.as_posix()}/..." for prefix in prefixes]


def uncovered_gated_packages(
    packages: Iterable[PurePath], manifest: ScopeManifest
) -> list[PurePath]:
    covered = [*chain.from_iterable(manifest.os_prefixes.values()), *manifest.linux_covered]
    return sorted(package for package in packages if not in_scope(package, covered))
```

- [ ] **Step 6: Run to verify pass**

Run: `bazel test --config=release //:clang_tidy_scope_test`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add .clang-tidy-scope.toml scripts/clang_tidy_scope.py scripts/clang_tidy_scope_test.py BUILD.bazel
git commit -m "feat: clang-tidy scope manifest and loader"
```

### Task 6: `--scope-os` and the coverage guard in the runner

**Files:**
- Modify: `scripts/clang_tidy_runner.py`
- Modify: `scripts/clang_tidy_runner_test.py`

**Interfaces:**
- Consumes: everything from Task 5; `_workspace_tree`, `_entry_relative_path`, `SOURCE_SUFFIXES` already in the runner.
- Produces: `_gated_packages(tree: _WorkspaceTree) -> list[PurePath]`; `run_workflow(..., scope_os: str | None = None)`; CLI `--scope-os OS`.

Behavior: the guard runs on **every** run (manifest present at `workspace/.clang-tidy-scope.toml`; if absent, skip the guard and reject `--scope-os`). With `--scope-os X`: build/refresh target patterns become the manifest's `X` prefixes (flags such as `--config=release` are kept, positional patterns replaced), and entries are filtered to those prefixes.

- [ ] **Step 1: Write the failing tests** (in `ClangTidyRunnerTest`)

```python
    def write_manifest(self, text: str) -> None:
        (self.root / ".clang-tidy-scope.toml").write_text(text)

    def test_gated_packages_need_c_sources_and_the_gating_keyword(self) -> None:
        (self.root / "pkg").mkdir()
        (self.root / "pkg" / "BUILD.bazel").write_text("x(target_compatible_with = [])\n")
        (self.root / "pkg" / "a.cpp").write_text("int a;\n")
        (self.root / "nosrc").mkdir()
        (self.root / "nosrc" / "BUILD.bazel").write_text("x(target_compatible_with = [])\n")
        (self.root / "plain").mkdir()
        (self.root / "plain" / "BUILD.bazel").write_text("x()\n")
        (self.root / "plain" / "b.cpp").write_text("int b;\n")
        tree = runner._workspace_tree(self.root)
        self.assertEqual([PurePath("pkg")], runner._gated_packages(tree))

    def test_guard_fails_for_a_gated_package_the_manifest_omits(self) -> None:
        self.write_manifest('[os.windows]\nprefixes = ["covered"]\n')
        (self.root / "gated").mkdir()
        (self.root / "gated" / "BUILD.bazel").write_text("x(target_compatible_with = [])\n")
        (self.root / "gated" / "a.cpp").write_text("int a;\n")
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        with (
            mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS),
            self.assertRaisesRegex(runner.WorkflowError, "gated"),
        ):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="linux",
                environ={},
                command_runner=lambda command, **kwargs: subprocess.CompletedProcess(command, 0),
            )

    def scoped_fixture(self) -> tuple[Path, Path]:
        self.write_manifest('[os.windows]\nprefixes = ["win"]\n')
        (self.root / "win").mkdir()
        windows_source = self.root / "win" / "w.cpp"
        windows_source.write_text("int w;\n")
        portable_source = self.root / "p.cpp"
        portable_source.write_text("int p;\n")
        self.write_database([windows_source, portable_source])
        return windows_source, portable_source

    def run_scoped(
        self, *, changed: bool = False, scope_os: str = "windows"
    ) -> tuple[list[list[str]], str]:
        commands: list[list[str]] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            return subprocess.CompletedProcess(command, 0, stdout="")

        output = StringIO()
        with (
            mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS),
            mock.patch.object(runner, "changed_files", return_value=[self.root / "p.h"]),
            redirect_stdout(output),
        ):
            runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="linux",
                environ={},
                command_runner=fake_run,
                build_args=[_CONFIG_RELEASE, "//..."],
                compdb_args=[_CONFIG_RELEASE],
                changed=changed,
                scope_os=scope_os,
            )
        return commands, output.getvalue()

    def test_scope_os_analyzes_only_that_oss_prefixes(self) -> None:
        self.scoped_fixture()
        _, output = self.run_scoped()
        self.assertIn("Analyzing 1 translation units", output)

    def test_scope_os_narrows_the_prebuild_and_refresh_targets(self) -> None:
        self.scoped_fixture()
        commands, _ = self.run_scoped()
        build = next(command for command in commands if command[:2] == ["bazel", "build"])
        self.assertEqual(["bazel", "build", "--keep_going", _CONFIG_RELEASE, "//win/..."], build)
        refresh = next(command for command in commands if command[0] == _UNIX_COMPDB_TOOL)
        self.assertEqual([_UNIX_COMPDB_TOOL, _CONFIG_RELEASE, "//win/..."], refresh)

    def test_scope_os_with_changed_header_outside_scope_skips_cleanly(self) -> None:
        self.scoped_fixture()
        _, output = self.run_scoped(changed=True)
        self.assertIn("no changed C/C++ translation units", output)

    def test_unknown_scope_os_is_an_error(self) -> None:
        self.scoped_fixture()
        with self.assertRaisesRegex(runner.WorkflowError, "scope.*plan9"):
            self.run_scoped(scope_os="plan9")

    def test_scope_os_without_a_manifest_is_an_error(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        with self.assertRaisesRegex(runner.WorkflowError, "scope manifest"):
            self.run_scoped()
```

(`test_scope_os_without_a_manifest_is_an_error` calls `run_scoped` with no manifest written; `run_scoped` itself does not write one.)

- [ ] **Step 2: Run to verify failure**

Run: `bazel test --config=release //:clang_tidy_runner_test`
Expected: FAIL (`_gated_packages` / `scope_os` do not exist).

- [ ] **Step 3: Implement in `scripts/clang_tidy_runner.py`**

Add `import clang_tidy_scope as scope_manifest` next to the other module import. After `load_project_entries`:

```python
def _gated_packages(tree: _WorkspaceTree) -> list[PurePath]:
    """Packages whose BUILD file gates targets by platform and that own C/C++ sources."""
    source_directories = {
        relative.parent for relative in tree.files if relative.suffix.lower() in SOURCE_SUFFIXES
    }
    packages: list[PurePath] = []
    for relative, path in tree.files.items():
        if relative.name not in ("BUILD", "BUILD.bazel") or relative.parent not in source_directories:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            continue
        if "target_compatible_with" in text:
            packages.append(relative.parent)
    return sorted(packages)


def _load_scope(workspace: Path) -> scope_manifest.ScopeManifest | None:
    path = workspace / scope_manifest.MANIFEST_NAME
    if not path.is_file():
        return None
    try:
        return scope_manifest.load_manifest(path)
    except scope_manifest.ScopeError as error:
        raise WorkflowError(str(error)) from error


def _retarget(args: Sequence[str], targets: Sequence[str]) -> list[str]:
    """Keep the flag arguments and replace every positional target pattern."""
    return [*(arg for arg in args if arg.startswith("-")), *targets]
```

In `run_workflow`: add `scope_os: str | None = None` after `profile`. Immediately after the mode checks and **before** `timings = PhaseTimings()`:

```python
    manifest = _load_scope(workspace)
    scope_prefixes: tuple[PurePath, ...] = ()
    if manifest is not None:
        uncovered = scope_manifest.uncovered_gated_packages(
            _gated_packages(_workspace_tree(workspace.resolve())), manifest
        )
        if uncovered:
            listed = ", ".join(path.as_posix() for path in uncovered)
            raise WorkflowError(
                f"platform-gated packages are not named in {scope_manifest.MANIFEST_NAME}: "
                f"{listed}; add each to [os.<name>] or [linux_covered]"
            )
    if scope_os is not None:
        if manifest is None:
            raise WorkflowError(f"--scope-os needs the scope manifest {scope_manifest.MANIFEST_NAME}")
        if scope_os not in manifest.os_prefixes:
            raise WorkflowError(
                f"scope manifest has no section for {scope_os!r}; "
                f"known: {', '.join(sorted(manifest.os_prefixes))}"
            )
        scope_prefixes = manifest.os_prefixes[scope_os]
        targets = scope_manifest.build_targets(scope_prefixes)
        build_args = _retarget(build_args, targets)
        compdb_args = _retarget(compdb_args, targets)
```

Inside the `with timings.phase("filter"):` block, right after `entries = load_project_entries(...)` add:

```python
        if scope_os is not None:
            tree = _workspace_tree(workspace.resolve())
            entries = [
                entry
                for entry in entries
                if (relative := _entry_relative_path(entry, tree)) is not None
                and scope_manifest.in_scope(relative, scope_prefixes)
            ]
```

Change the skip condition so a scoped run with nothing in scope also skips cleanly instead of failing: replace `if changed and not entries:` with `if (changed or scope_os is not None) and not entries:` and its message stays "no changed C/C++ translation units to analyze, skipping." (the test asserts that text; for the unscoped-and-empty case `load_project_entries` already raised earlier).

Note `load_project_entries` raises when the DB has no workspace TUs at all; that is still an error under `--scope-os` (a Windows run whose compile DB lacks Windows sources is broken).

In `main`: `parser.add_argument("--scope-os")` and pass `scope_os=args.scope_os`.

- [ ] **Step 4: Run to verify pass**

Run: `bazel test --config=release //:clang_tidy_runner_test //:clang_tidy_scope_test`
Expected: PASS.

- [ ] **Step 5: Run the guard against the real repo**

Run: `bazel run --config=release //:clang_tidy_report_changed`
Expected: no "platform-gated packages are not named" error. If it names a package, decide per the manifest header comment and add it, then re-run. (At planning time the gated C++ packages were `src/platform/desktop/{common/serial, common/serial/direct/{unix,windows}, unix/j2534{,/driver,/testing}, windows/j2534{,/j2534_bridge_host}}` and `tests{,/force_asserts}`; all are covered by Task 5's manifest.)

- [ ] **Step 6: Commit**

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py
git commit -m "feat: scope clang-tidy to one OS's exclusive code, guard platform-gated packages"
```

### Task 7: Document the scope and submit slice 2

**Files:**
- Modify: `docs/coding-style.md` (Static analysis section)

- [ ] **Step 1:** Add a bullet under `## Static analysis`:

```markdown
- Platform-gated code is listed in the clang-tidy scope manifest
  (`.clang-tidy-scope.toml`): Linux analyzes everything it can build, Windows
  only its exclusive code (`-- --scope-os windows`). The runner fails when a
  `BUILD.bazel` gates targets by platform and no manifest prefix covers it.
```

(Per the repo rule, write the manifest name as a link to the file, e.g. `[scope manifest](../.clang-tidy-scope.toml)`, not a backticked path, if lychee flags it.)

- [ ] **Step 2: Verify and commit**

Run: `prek run --all-files`
Expected: pass.

```bash
git add docs/coding-style.md
git commit -m "docs: describe the clang-tidy scope manifest"
```

---

## PART 3 — Parallel clang-tidy job (PR 3)

### Task 8: Move tidy into its own Linux + Windows job

**Files:**
- Modify: `.github/workflows/pr.yml`
- Modify: `docs/coding-style.md` (one sentence)
- Delete (final commit of the stack, per repo convention): `docs/superpowers/specs/2026-10-03-clang-tidy-ci-speedup-design.md`, `docs/superpowers/plans/2026-10-03-clang-tidy-ci-speedup.md`

- [ ] **Step 1: Remove tidy from the `bazel` job**

Delete the final step of the `bazel` job:

```yaml
      - name: clang-tidy report
        run: |
          bazel run --config=release //:clang_tidy_report_changed -- --profile
```

Delete the Linux prerequisites step (it only installs clang-tidy):

```yaml
      - name: Install Bazel prerequisites (Linux)
        if: runner.os == 'Linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y clang-tidy
```

Leave the Windows (`choco install llvm`) and macOS (`brew install llvm`) prerequisite steps in place for now: they may serve more than tidy, and removing them is a follow-up once a run proves it safe.

- [ ] **Step 2: Add the `clang-tidy` job** (after the `bazel` job)

```yaml
  clang-tidy:
    name: clang-tidy (${{ matrix.os }})
    needs: pre-commit
    runs-on: ${{ matrix.os }}
    timeout-minutes: 30
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-26.04
            scope: ''
          - os: windows-latest
            scope: '--scope-os windows'
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          fetch-depth: 0

      - uses: bazel-contrib/setup-bazel@c5acdfb288317d0b5c0bbd7a396a3dc868bb0f86 # 0.19.0
        with:
          bazelisk-cache: true
          repository-cache: true
          cache-save: false

      - uses: ./.github/actions/buildbuddy
        with:
          api-key: ${{ secrets.BUILDBUDDY_API_KEY }}

      - name: Install Qt for host tools (Windows)
        if: runner.os == 'Windows'
        uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: ${{ env.QT_VERSION }}
          arch: 'win64_msvc2022_64'
          modules: ${{ env.QT_MODULES }}
          add-tools-to-path: true

      - name: Install Qt for host tools (Linux)
        if: runner.os == 'Linux'
        uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: ${{ env.QT_VERSION }}
          modules: ${{ env.QT_MODULES }}

      - name: Install clang-tidy (Windows)
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          choco install llvm -y --no-progress
          "C:\Program Files\LLVM\bin" | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append

      - name: Install clang-tidy (Linux)
        if: runner.os == 'Linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y clang-tidy

      - name: clang-tidy report
        run: |
          bazel run --config=release //:clang_tidy_report_changed -- --profile ${{ matrix.scope }}
```

- [ ] **Step 3: Validate the workflow syntax**

Run: `prek run --all-files` and, if installed, `actionlint .github/workflows/pr.yml`
Expected: pass. (`prek` runs the repo's own checks; actionlint is optional.)

- [ ] **Step 4: Update the docs sentence**

In `docs/coding-style.md` Static analysis, add: "In CI, clang-tidy runs as its own `clang-tidy` job (Linux, plus Windows for Windows-exclusive code), in parallel with the Bazel build and test job."

- [ ] **Step 5: Drop the spec and plan, commit**

```bash
git rm docs/superpowers/specs/2026-10-03-clang-tidy-ci-speedup-design.md docs/superpowers/plans/2026-10-03-clang-tidy-ci-speedup.md
git add .github/workflows/pr.yml docs/coding-style.md
git commit -m "ci: run clang-tidy as a parallel Linux+Windows job"
```

- [ ] **Step 6: Required-check settings (manual, tell the user)**

Branch protection must stop requiring the removed behaviors and start requiring the new checks `clang-tidy (ubuntu-26.04)` and `clang-tidy (windows-latest)`. Nothing in the repo changes this; the user does it in repository settings after the PR merges. Do not assume it is done.

- [ ] **Step 7: Verify on the PR**

After the PR's first run, confirm in the Actions UI: (a) the `bazel` job no longer has a tidy step; (b) `clang-tidy (windows-latest)` analyzes a handful of units and its profile summary shows `prebuild` as the dominant phase, if the narrowed `//src/platform/desktop/windows/... //src/platform/desktop/common/serial/... //tests/...` build is still slow; (c) the Windows compile DB refresh accepted the narrowed target patterns. If Hedron rejects them, revert `_retarget` for the refresh only (keep it for the prebuild) and note it in the PR.

---

## Self-Review

**Spec coverage.** Part 1 (phase timers, profile flag, summary, CI-on) → Tasks 2–4; spec amendments for the aggregated report → Task 1. Part 2 (manifest, per-OS behavior, narrowed prebuild/refresh, guard) → Tasks 5–7; the macOS finding → Task 1 amendment and manifest. Part 3 (parallel job, `bazel` job loses tidy, cold-cache note) → Task 8. Slicing (three stacked PRs) → Global Constraints and the PART headers. Spec risk "Hedron with narrowed patterns" → Task 8 Step 7; "prebuild may be slow" → Task 4 Step 4 and Task 8 Step 7. Success criteria are observable in Task 8 Step 7.

**Placeholder scan.** No TBD/TODO; every code step shows code. The one conditional is the exports_files fallback in Task 3 Step 4, with the exact edit.

**Type consistency.** `PhaseTimings.durations`/`.phase` (Task 3) are used by `_emit_profile` and `run_workflow` consistently. `parse_check_profile`/`render_markdown` signatures match between Tasks 2 and 3. `ScopeManifest`, `in_scope`, `build_targets`, `uncovered_gated_packages`, `MANIFEST_NAME` and `ScopeError` (Task 5) match their uses in Task 6 (`scope_manifest.*`). `run_workflow` keywords `profile` (Task 3) and `scope_os` (Task 6) match the CLI flags `--profile`/`--scope-os` and the workflow's use in Tasks 4 and 8.

**Review Focus.** Each line maps to a test: multi-table/instr-column parse and empty-report note (Task 2), unwritable summary (Task 3), guard with/without sources and uncovered package (Tasks 5–6), unknown OS and unsafe manifest paths (Tasks 5–6), changed header outside scope (Task 6).
