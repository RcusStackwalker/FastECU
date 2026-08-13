# clang-tidy changed-files gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `bazel run //:clang_tidy_fix` usable as a pre-PR local gate by scoping analysis to changed files and quieting its output, then swap `pr.yml`'s per-PR clang-tidy check to the same changed-files scope.

**Architecture:** Add a `--changed` flag to the existing `scripts/clang_tidy_runner.py` CLI that filters the Bazel compile database down to translation units affected by a git diff against `origin/master` before handing them to `run-clang-tidy`; change the runner's subprocess handling to capture build/tidy output and only print it on failure; expose the new scope as two new Bazel targets; repoint `pr.yml` at the changed-files report target and drop the full-repo report from CI (it remains a manual target).

**Tech Stack:** Python 3 (`scripts/clang_tidy_runner.py`, stdlib `subprocess`/`json`/`pathlib` + `pyyaml`), Bazel `py_binary`/`py_test`, GitHub Actions YAML.

## Global Constraints

- The diff base is fixed at `origin/master` (via `git merge-base HEAD origin/master`) — no `--base` override flag.
- Omitting `--changed` must reproduce today's full-`//...` behavior exactly; existing `clang_tidy_report`/`clang_tidy_fix` targets are not modified.
- A changed header (`.h`/`.hpp`) with no changed source falls back to co-located sources in the same directory: an entry whose filename stem equals the header's stem, or `<stem>_test`, is included. A header with no co-located source produces a printed note, not an error.
- An empty filtered translation-unit set skips `run-clang-tidy` entirely and exits 0.
- `pr.yml`'s changed-files check is **report mode only** (never mutates files in CI) and keeps running once per OS in the existing 3-way matrix (windows-latest, macos-latest, ubuntu-26.04).
- Full-repo `clang_tidy_report` is dropped from CI entirely (not moved to `release.yml`); it remains a manual/local target.
- All new runner behavior must be covered by `scripts/clang_tidy_runner_test.py` using its existing fake-`command_runner` injection pattern — no real subprocess or network calls in tests.

---

## Task 1: Quiet the prebuild and post-fix build subprocess output

**Files:**
- Modify: `scripts/clang_tidy_runner.py` (functions `_prebuild`, `_post_fix_build`; add helper `_run_quiet`)
- Test: `scripts/clang_tidy_runner_test.py`

**Interfaces:**
- Produces: `_run_quiet(command_runner: CommandRunner, command: list[str], workspace: Path) -> subprocess.CompletedProcess[str]` — runs `command` with combined stdout+stderr captured as text (`stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True`), raising `WorkflowError` only on `OSError` (exec failure). Callers inspect `.returncode`/`.stdout` themselves.

Today `_prebuild` and `_post_fix_build` call `_run`, which lets the subprocess's stdout/stderr stream straight to the console every time — including on a clean, successful build. This task captures that output instead and only prints it when the build actually fails.

- [ ] **Step 1: Write the failing tests**

Add to `scripts/clang_tidy_runner_test.py` (near the other `test_prebuild_*` tests):

```python
    def test_prebuild_success_output_is_suppressed(self) -> None:
        commands, fake_run = self.prebuild_fixture()

        def noisy_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            result = fake_run(command, **kwargs)
            if command[:2] == ["bazel", "build"]:
                return subprocess.CompletedProcess(command, result.returncode, stdout="noisy build log\n")
            return result

        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            output = StringIO()
            with redirect_stdout(output):
                runner.run_workflow(
                    mode="report",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=noisy_run,
                    build_args=[_CONFIG_RELEASE, _FASTECU_TARGET],
                )

        self.assertNotIn("noisy build log", output.getvalue())

    def test_prebuild_failure_output_is_surfaced(self) -> None:
        commands, fake_run = self.prebuild_fixture(bazel_build_returncode=3)

        def noisy_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            result = fake_run(command, **kwargs)
            if command[:2] == ["bazel", "build"]:
                return subprocess.CompletedProcess(command, result.returncode, stdout="prebuild trouble\n")
            return result

        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            output = StringIO()
            with redirect_stdout(output):
                runner.run_workflow(
                    mode="report",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=noisy_run,
                    build_args=[_FASTECU_TARGET],
                )

        self.assertIn("prebuild trouble", output.getvalue())

    def test_post_fix_build_failure_output_is_surfaced(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])
        build_count = 0
        tools = runner.Tools(
            clang_tidy="/llvm/bin/clang-tidy",
            run_clang_tidy="/llvm/bin/run-clang-tidy",
            clang_apply_replacements="/llvm/bin/clang-apply-replacements",
        )

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            nonlocal build_count
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            if command[:2] == ["bazel", "build"]:
                build_count += 1
                if build_count == 2:
                    return subprocess.CompletedProcess(command, 9, stdout="post-fix build broke\n")
                return subprocess.CompletedProcess(command, 0)
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner, "discover_tools", return_value=tools):
            output = StringIO()
            with (
                redirect_stdout(output),
                self.assertRaisesRegex(runner.WorkflowError, "post-fix Bazel build failed.*9"),
            ):
                runner.run_workflow(
                    mode="fix",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=fake_run,
                    build_args=[_CONFIG_RELEASE, "//..."],
                )

        self.assertIn("post-fix build broke", output.getvalue())
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: FAIL — `test_prebuild_success_output_is_suppressed` and `test_prebuild_failure_output_is_surfaced` fail because today's `_prebuild` streams via `_run`, which doesn't capture the fake's `stdout` at all (nothing printed either way, so the "suppressed" test passes by accident but the "surfaced" test fails); `test_post_fix_build_failure_output_is_surfaced` fails because `_post_fix_build` never prints captured output before raising.

- [ ] **Step 3: Implement `_run_quiet` and use it in `_prebuild`/`_post_fix_build`**

Add this helper right after the existing `_run` function in `scripts/clang_tidy_runner.py`:

```python
def _run_quiet(
    command_runner: CommandRunner,
    command: list[str],
    workspace: Path,
) -> subprocess.CompletedProcess[str]:
    """Run a command, capturing combined output instead of streaming it live."""
    try:
        return command_runner(
            command,
            cwd=workspace,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except OSError as error:
        raise WorkflowError(f"could not execute {command[0]}: {error}") from error
```

Replace `_prebuild`'s body:

```python
    if not build_args:
        return
    print("Building analyzed targets so generated headers exist before analysis.")
    code = _run(command_runner, ["bazel", "build", "--keep_going", *build_args], workspace)
    if code:
        print(
            f"clang-tidy: warning: prebuild exited with code {code}; "
            "some generated headers may be stale or missing.",
            file=sys.stderr,
        )
```

with:

```python
    if not build_args:
        return
    print("Building analyzed targets so generated headers exist before analysis.")
    result = _run_quiet(command_runner, ["bazel", "build", "--keep_going", *build_args], workspace)
    if result.returncode:
        if result.stdout:
            print(result.stdout, end="")
        print(
            f"clang-tidy: warning: prebuild exited with code {result.returncode}; "
            "some generated headers may be stale or missing.",
            file=sys.stderr,
        )
```

Replace `_post_fix_build`'s body:

```python
    if not build_args:
        return
    print("Building analyzed targets after applying clang-tidy fixes.")
    code = _run(command_runner, ["bazel", "build", *build_args], workspace)
    if code:
        raise WorkflowError(f"post-fix Bazel build failed with exit code {code}")
```

with:

```python
    if not build_args:
        return
    print("Building analyzed targets after applying clang-tidy fixes.")
    result = _run_quiet(command_runner, ["bazel", "build", *build_args], workspace)
    if result.returncode:
        if result.stdout:
            print(result.stdout, end="")
        raise WorkflowError(f"post-fix Bazel build failed with exit code {result.returncode}")
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: PASS — all tests, including the three new ones and every pre-existing `test_prebuild_*`/`test_fix_propagates_post_replacement_build_failure` test (command construction is unchanged, only how output is captured).

- [ ] **Step 5: Commit**

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py
git commit -m "refactor(clang-tidy): capture build output, print only on failure"
```

---

## Task 2: Quiet run-clang-tidy's own output and print a terse summary

**Files:**
- Modify: `scripts/clang_tidy_runner.py` (function `run_workflow`; add helper `_count_diagnostics`)
- Test: `scripts/clang_tidy_runner_test.py`

**Interfaces:**
- Consumes: `_run_quiet` from Task 1.
- Produces: `_count_diagnostics(fixes_directory: Path) -> int` — sums `len(Diagnostics)` across every `*.yaml` in `fixes_directory`, tolerating malformed/missing documents by skipping them (this count is informational only; `normalize_replacements` remains the strict validator used before applying fixes).

Today, `report` mode never exports fixes (so there's no diagnostic count available) and `run-clang-tidy`'s raw stdout — which includes clang's own "N warnings generated" boilerplate per translation unit — streams to the console unconditionally. This task always exports fixes (in both modes, purely for counting in `report` mode) and captures `run-clang-tidy`'s output, printing it only when the run fails.

- [ ] **Step 1: Write the failing tests**

Add to `scripts/clang_tidy_runner_test.py`:

```python
    def test_count_diagnostics_sums_across_yaml_files(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()
        self.write_fixes(
            fixes_directory,
            "a.yaml",
            [self.diagnostic("first", []), self.diagnostic("second", [])],
        )
        self.write_fixes(fixes_directory, "b.yaml", [self.diagnostic("third", [])])

        self.assertEqual(3, runner._count_diagnostics(fixes_directory))

    def test_count_diagnostics_is_zero_for_empty_directory(self) -> None:
        fixes_directory = Path(self.temp_dir.name) / "fixes"
        fixes_directory.mkdir()

        self.assertEqual(0, runner._count_diagnostics(fixes_directory))

    def test_analysis_success_prints_terse_summary(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            if command[0] == "/llvm/bin/run-clang-tidy":
                return subprocess.CompletedProcess(command, 0, stdout="1 warning generated.\n")
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            output = StringIO()
            with redirect_stdout(output):
                runner.run_workflow(
                    mode="report",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=fake_run,
                )

        printed = output.getvalue()
        self.assertNotIn("1 warning generated", printed)
        self.assertIn("clang-tidy: 1 files clean, 0 findings", printed)

    def test_analysis_failure_prints_diagnostics_and_finding_count(self) -> None:
        source = self.root / _MAIN_CPP
        source.write_text("int main() { return 0; }\n")
        self.write_database([source])

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            if "-export-fixes" in command:
                fixes_directory = Path(command[command.index("-export-fixes") + 1])
                replacement = self.replacement(source, 0, 0, "// fixed\n")
                self.write_fixes(
                    fixes_directory,
                    "fix.yaml",
                    [self.diagnostic("readability-fix", [replacement])],
                )
                return subprocess.CompletedProcess(
                    command, 1, stdout="main.cpp:1:1: warning: fix me [readability-fix]\n"
                )
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            output = StringIO()
            with (
                redirect_stdout(output),
                self.assertRaisesRegex(
                    runner.WorkflowError, r"1 findings.*failed with exit code 1"
                ),
            ):
                runner.run_workflow(
                    mode="report",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=fake_run,
                )

        self.assertIn("fix me [readability-fix]", output.getvalue())
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: FAIL — `runner._count_diagnostics` doesn't exist yet (`AttributeError`); the summary/failure-text tests fail because `report` mode never passes `-export-fixes` today and `run-clang-tidy`'s raw stdout still streams unconditionally.

- [ ] **Step 3: Implement the count helper and quiet the analysis invocation**

Add near `normalize_replacements` in `scripts/clang_tidy_runner.py`:

```python
def _count_diagnostics(fixes_directory: Path) -> int:
    """Count exported diagnostics across a fixes directory.

    Lenient by design: report mode discards these files after counting them,
    so a malformed document is skipped rather than raising. Strict validation
    happens in normalize_replacements, which fix mode always runs before
    applying anything.
    """
    total = 0
    for path in sorted(fixes_directory.glob("*.yaml")):
        document = yaml.safe_load(path.read_text())
        if not isinstance(document, dict):
            continue
        diagnostics = document.get("Diagnostics")
        if isinstance(diagnostics, list):
            total += len(diagnostics)
    return total
```

In `run_workflow`, replace this block:

```python
        fixes_directory = Path(directory) / "fixes"
        if mode == "fix":
            fixes_directory.mkdir()
            command.extend(["-export-fixes", str(fixes_directory) + os.sep])
        print(f"Running clang-tidy in {mode} mode over {len(entries)} translation units.")
        tidy_code = _run(command_runner, command, workspace)
        if mode == "fix":
            assert tools.clang_apply_replacements is not None
            normalize_replacements(fixes_directory, workspace)
            apply_code = _run(
                command_runner,
                [
                    tools.clang_apply_replacements,
                    str(fixes_directory),
                ],
                workspace,
            )
            if apply_code:
                raise WorkflowError(f"replacement application failed with exit code {apply_code}")
            _post_fix_build(command_runner, build_args, workspace)
        if tidy_code:
            detail = f"run-clang-tidy failed with exit code {tidy_code}"
            if mode == "fix":
                detail += "; exported fixes were applied before reporting the failure"
            raise WorkflowError(detail)
    return 0
```

with:

```python
        fixes_directory = Path(directory) / "fixes"
        fixes_directory.mkdir()
        command.extend(["-export-fixes", str(fixes_directory) + os.sep])
        print(f"Analyzing {len(entries)} translation units in {mode} mode.")
        tidy_result = _run_quiet(command_runner, command, workspace)
        tidy_code = tidy_result.returncode
        finding_count = _count_diagnostics(fixes_directory)
        if mode == "fix":
            assert tools.clang_apply_replacements is not None
            normalize_replacements(fixes_directory, workspace)
            apply_code = _run(
                command_runner,
                [
                    tools.clang_apply_replacements,
                    str(fixes_directory),
                ],
                workspace,
            )
            if apply_code:
                if tidy_result.stdout:
                    print(tidy_result.stdout, end="")
                raise WorkflowError(f"replacement application failed with exit code {apply_code}")
            _post_fix_build(command_runner, build_args, workspace)
        if tidy_code:
            if tidy_result.stdout:
                print(tidy_result.stdout, end="")
            detail = f"{finding_count} findings; run-clang-tidy failed with exit code {tidy_code}"
            if mode == "fix":
                detail += "; exported fixes were applied before reporting the failure"
            raise WorkflowError(detail)
        print(f"clang-tidy: {len(entries)} files clean, 0 findings")
    return 0
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: PASS — all tests, including the four new ones. `test_analysis_failure_is_propagated` and `test_fix_applies_exported_replacements_before_propagating_analysis_failure` (which assert on `WorkflowError` message substrings `"failed.*7"` and `"failed.*1.*fixes were applied"`) still match because those substrings still appear in the new, longer message.

- [ ] **Step 5: Commit**

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py
git commit -m "refactor(clang-tidy): quiet run-clang-tidy output, print terse pass/fail summary"
```

---

## Task 3: Git-based changed-file detection

**Files:**
- Modify: `scripts/clang_tidy_runner.py` (add `_git_lines`, `changed_files`)
- Test: `scripts/clang_tidy_runner_test.py`

**Interfaces:**
- Produces: `changed_files(workspace: Path, command_runner: CommandRunner) -> list[Path]` — resolved, deduplicated, sorted absolute paths of files changed relative to `origin/master` (committed + staged + unstaged + untracked), filtered to files that currently exist on disk.

- [ ] **Step 1: Write the failing tests**

Add to `scripts/clang_tidy_runner_test.py`:

```python
    def test_changed_files_unions_git_sources(self) -> None:
        committed = self.root / "committed.cpp"
        committed.write_text("// committed\n")
        staged = self.root / "staged.h"
        staged.write_text("// staged\n")
        untracked = self.root / "untracked.cpp"
        untracked.write_text("// untracked\n")
        # deleted.cpp is intentionally never created: a deleted file can still
        # show up in `git diff --name-only` and must be filtered out.

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if command == ["git", "merge-base", "HEAD", "origin/master"]:
                return subprocess.CompletedProcess(command, 0, stdout="abc123\n")
            if command == ["git", "diff", "--name-only", "abc123..HEAD"]:
                return subprocess.CompletedProcess(command, 0, stdout="committed.cpp\ndeleted.cpp\n")
            if command == ["git", "diff", "--name-only"]:
                return subprocess.CompletedProcess(command, 0, stdout="")
            if command == ["git", "diff", "--name-only", "--cached"]:
                return subprocess.CompletedProcess(command, 0, stdout="staged.h\n")
            if command == ["git", "ls-files", "--others", "--exclude-standard"]:
                return subprocess.CompletedProcess(command, 0, stdout="untracked.cpp\n")
            raise AssertionError(f"unexpected command: {command}")

        result = runner.changed_files(self.root, fake_run)

        self.assertEqual(
            [committed.resolve(), staged.resolve(), untracked.resolve()],
            result,
        )

    def test_changed_files_reports_merge_base_failure(self) -> None:
        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(command, 128, stderr="fatal: no such ref\n")

        with self.assertRaisesRegex(runner.WorkflowError, r"git merge-base.*128.*no such ref"):
            runner.changed_files(self.root, fake_run)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: FAIL — `runner.changed_files` doesn't exist yet (`AttributeError`).

- [ ] **Step 3: Implement `_git_lines` and `changed_files`**

Add to `scripts/clang_tidy_runner.py`, after `find_executable`:

```python
def _git_lines(command_runner: CommandRunner, args: Sequence[str], workspace: Path) -> list[str]:
    command = ["git", *args]
    try:
        result = command_runner(
            command,
            cwd=workspace,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise WorkflowError(f"could not execute {command[0]}: {error}") from error
    if result.returncode:
        raise WorkflowError(
            f"git {' '.join(args)} failed with exit code {result.returncode}: "
            f"{(result.stderr or '').strip()}"
        )
    return [line for line in (result.stdout or "").splitlines() if line]


def changed_files(workspace: Path, command_runner: CommandRunner) -> list[Path]:
    """Files changed relative to origin/master: committed, staged, unstaged, and new.

    Deleted files (present in a diff but no longer on disk) are dropped --
    there's nothing left for clang-tidy to analyze.
    """
    merge_base = _git_lines(command_runner, ["merge-base", "HEAD", "origin/master"], workspace)
    if not merge_base:
        raise WorkflowError("git merge-base HEAD origin/master produced no output")
    base = merge_base[0]

    relative_paths: set[str] = set()
    relative_paths.update(
        _git_lines(command_runner, ["diff", "--name-only", f"{base}..HEAD"], workspace)
    )
    relative_paths.update(_git_lines(command_runner, ["diff", "--name-only"], workspace))
    relative_paths.update(
        _git_lines(command_runner, ["diff", "--name-only", "--cached"], workspace)
    )
    relative_paths.update(
        _git_lines(command_runner, ["ls-files", "--others", "--exclude-standard"], workspace)
    )

    candidates = (workspace / path for path in relative_paths)
    return sorted({path.resolve() for path in candidates if path.is_file()})
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: PASS — all tests, including the two new ones.

- [ ] **Step 5: Commit**

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py
git commit -m "feat(clang-tidy): add git-based changed-file detection"
```

---

## Task 4: Filter compile-database entries to changed translation units

**Files:**
- Modify: `scripts/clang_tidy_runner.py` (add `HEADER_SUFFIXES`, `_entry_source_path`, `filter_changed_entries`; refactor `load_project_entries` to use `_entry_source_path`)
- Test: `scripts/clang_tidy_runner_test.py`

**Interfaces:**
- Produces:
  - `HEADER_SUFFIXES = frozenset((".h", ".hpp"))`
  - `_entry_source_path(entry: dict[str, object], root: Path) -> Path` — resolves a compile-database entry's absolute source path the same way `load_project_entries` already does (directory-relative `file`, resolved against `root` if `directory` itself is relative).
  - `filter_changed_entries(entries: list[dict[str, object]], changed: Sequence[Path], root: Path) -> tuple[list[dict[str, object]], list[str]]` — returns `(matched_entries, notes)`. Direct source-suffix matches map by path; header-suffix changes fall back to co-located sources (exact stem or `<stem>_test`) in the same directory, producing a note when none exist.
- Consumes: `SOURCE_SUFFIXES` (existing constant).

- [ ] **Step 1: Write the failing tests**

Add to `scripts/clang_tidy_runner_test.py`:

```python
    def test_filter_changed_entries_matches_source_directly(self) -> None:
        pkg = self.root / "pkg"
        pkg.mkdir()
        source = pkg / "foo.cpp"
        source.write_text("// foo\n")
        self.write_database([source])
        entries = runner.load_project_entries(self.root, self.root / "compile_commands.json")

        matched, notes = runner.filter_changed_entries(entries, [source], self.root)

        self.assertEqual([str(source)], [entry["file"] for entry in matched])
        self.assertEqual([], notes)

    def test_filter_changed_entries_falls_back_to_colocated_source(self) -> None:
        pkg = self.root / "pkg"
        pkg.mkdir()
        source = pkg / "foo.cpp"
        source.write_text("// foo\n")
        test_source = pkg / "foo_test.cpp"
        test_source.write_text("// foo test\n")
        header = pkg / "foo.h"
        header.write_text("#pragma once\n")
        self.write_database([source, test_source])
        entries = runner.load_project_entries(self.root, self.root / "compile_commands.json")

        matched, notes = runner.filter_changed_entries(entries, [header], self.root)

        self.assertEqual(
            {str(source), str(test_source)},
            {entry["file"] for entry in matched},
        )
        self.assertEqual([], notes)

    def test_filter_changed_entries_notes_uncolocated_header(self) -> None:
        pkg = self.root / "pkg"
        pkg.mkdir()
        source = pkg / "other.cpp"
        source.write_text("// other\n")
        header = pkg / "standalone.h"
        header.write_text("#pragma once\n")
        self.write_database([source])
        entries = runner.load_project_entries(self.root, self.root / "compile_commands.json")

        matched, notes = runner.filter_changed_entries(entries, [header], self.root)

        self.assertEqual([], matched)
        self.assertEqual(1, len(notes))
        self.assertIn(str(header), notes[0])

    def test_filter_changed_entries_ignores_non_cpp_changes(self) -> None:
        source = self.root / "foo.cpp"
        source.write_text("// foo\n")
        self.write_database([source])
        entries = runner.load_project_entries(self.root, self.root / "compile_commands.json")
        doc = self.root / "README.md"
        doc.write_text("# readme\n")

        matched, notes = runner.filter_changed_entries(entries, [doc], self.root)

        self.assertEqual([], matched)
        self.assertEqual([], notes)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: FAIL — `runner.filter_changed_entries` doesn't exist yet (`AttributeError`).

- [ ] **Step 3: Implement the filter, extracting the shared path-resolution helper**

Add `HEADER_SUFFIXES` next to `SOURCE_SUFFIXES` near the top of `scripts/clang_tidy_runner.py`:

```python
SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx"))
HEADER_SUFFIXES = frozenset((".h", ".hpp"))
```

Add `_entry_source_path` right before `load_project_entries`:

```python
def _entry_source_path(entry: dict[str, object], root: Path) -> Path:
    """Resolve a compile-database entry's absolute source path.

    Callers must already have validated that `directory` and `file` are
    present strings (load_project_entries does this before an entry survives
    into its returned list).
    """
    directory = Path(str(entry["directory"]))
    if not directory.is_absolute():
        directory = root / directory
    source = Path(str(entry["file"]))
    if not source.is_absolute():
        source = directory / source
    return source.resolve()
```

Refactor `load_project_entries`'s loop body from:

```python
        directory_value = entry.get("directory")
        file_value = entry.get("file")
        if not isinstance(directory_value, str) or not isinstance(file_value, str):
            raise WorkflowError("compilation database is malformed: entry lacks directory or file")

        directory = Path(directory_value)
        if not directory.is_absolute():
            directory = root / directory
        source = Path(file_value)
        if not source.is_absolute():
            source = directory / source
        source = source.resolve()
        try:
```

to:

```python
        directory_value = entry.get("directory")
        file_value = entry.get("file")
        if not isinstance(directory_value, str) or not isinstance(file_value, str):
            raise WorkflowError("compilation database is malformed: entry lacks directory or file")

        source = _entry_source_path(entry, root)
        try:
```

Add `filter_changed_entries` after `load_project_entries`:

```python
def filter_changed_entries(
    entries: list[dict[str, object]],
    changed: Sequence[Path],
    root: Path,
) -> tuple[list[dict[str, object]], list[str]]:
    """Narrow `entries` to translation units affected by `changed`.

    Source files match by path directly. A changed header with no changed
    source file falls back to co-located sources in the same directory
    (foo.h -> foo.cpp, foo_test.cpp), matching this repo's package-owned test
    convention. A header with no co-located source in `entries` produces a
    note instead of a match; this design does not trace transitive includers.
    """
    by_path: dict[Path, dict[str, object]] = {}
    by_directory: dict[Path, list[tuple[str, dict[str, object]]]] = {}
    for entry in entries:
        source = _entry_source_path(entry, root)
        by_path[source] = entry
        by_directory.setdefault(source.parent, []).append((source.stem, entry))

    matched: dict[Path, dict[str, object]] = {}
    notes: list[str] = []
    for path in changed:
        resolved = path.resolve()
        suffix = resolved.suffix.lower()
        if suffix in SOURCE_SUFFIXES:
            entry = by_path.get(resolved)
            if entry is not None:
                matched[resolved] = entry
        elif suffix in HEADER_SUFFIXES:
            stem = resolved.stem
            candidates = by_directory.get(resolved.parent, [])
            found = [
                entry
                for candidate_stem, entry in candidates
                if candidate_stem in (stem, f"{stem}_test")
            ]
            if found:
                for entry in found:
                    matched[_entry_source_path(entry, root)] = entry
            else:
                notes.append(
                    f"clang-tidy: {resolved} changed with no co-located source in the "
                    "compile database; its includers were not checked."
                )

    return [matched[key] for key in sorted(matched)], notes
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: PASS — all tests, including the four new ones and every pre-existing `test_database_*` test (the `load_project_entries` refactor is behavior-preserving).

- [ ] **Step 5: Commit**

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py
git commit -m "feat(clang-tidy): filter compile database to changed translation units"
```

---

## Task 5: Wire `--changed` through the CLI and `run_workflow`

**Files:**
- Modify: `scripts/clang_tidy_runner.py` (function `run_workflow`, function `main`)
- Test: `scripts/clang_tidy_runner_test.py`

**Interfaces:**
- Consumes: `changed_files` (Task 3), `filter_changed_entries` (Task 4).
- Produces: `run_workflow(..., changed: bool = False) -> int` (new keyword parameter); CLI flag `--changed` (`action="store_true"`).

- [ ] **Step 1: Write the failing tests**

Add to `scripts/clang_tidy_runner_test.py`:

```python
    def test_changed_mode_filters_entries_before_analysis(self) -> None:
        changed_source = self.root / "changed.cpp"
        changed_source.write_text("int changed;\n")
        unrelated_source = self.root / "unrelated.cpp"
        unrelated_source.write_text("int unrelated;\n")
        self.write_database([changed_source, unrelated_source])
        analyzed_files: list[str] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if command == ["git", "merge-base", "HEAD", "origin/master"]:
                return subprocess.CompletedProcess(command, 0, stdout="abc123\n")
            if command == ["git", "diff", "--name-only", "abc123..HEAD"]:
                return subprocess.CompletedProcess(command, 0, stdout="changed.cpp\n")
            if command[:2] == ["git", "diff"]:
                return subprocess.CompletedProcess(command, 0, stdout="")
            if command == ["git", "ls-files", "--others", "--exclude-standard"]:
                return subprocess.CompletedProcess(command, 0, stdout="")
            if command == ["xcrun", "--show-sdk-path"]:
                return subprocess.CompletedProcess(command, 0, stdout="/SDK/MacOSX.sdk\n")
            if command[0] == _UNIX_TOOLS.run_clang_tidy:
                compdb_dir = Path(command[command.index("-p") + 1])
                database = json.loads((compdb_dir / "compile_commands.json").read_text())
                analyzed_files.extend(entry["file"] for entry in database)
                return subprocess.CompletedProcess(command, 0)
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            result = runner.run_workflow(
                mode="report",
                workspace=self.root,
                compdb_tool=_UNIX_COMPDB_TOOL,
                platform_name="darwin",
                environ={},
                command_runner=fake_run,
                changed=True,
            )

        self.assertEqual(0, result)
        self.assertEqual([str(changed_source)], analyzed_files)

    def test_changed_mode_skips_analysis_when_nothing_matches(self) -> None:
        source = self.root / "unrelated.cpp"
        source.write_text("int unrelated;\n")
        self.write_database([source])
        tidy_invoked = False

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            nonlocal tidy_invoked
            if command == ["git", "merge-base", "HEAD", "origin/master"]:
                return subprocess.CompletedProcess(command, 0, stdout="abc123\n")
            if command[:2] == ["git", "diff"] or command[:2] == ["git", "ls-files"]:
                return subprocess.CompletedProcess(command, 0, stdout="")
            if command[0] == _UNIX_TOOLS.run_clang_tidy:
                tidy_invoked = True
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner, "discover_tools", return_value=_UNIX_TOOLS):
            output = StringIO()
            with redirect_stdout(output):
                result = runner.run_workflow(
                    mode="report",
                    workspace=self.root,
                    compdb_tool=_UNIX_COMPDB_TOOL,
                    platform_name="darwin",
                    environ={},
                    command_runner=fake_run,
                    changed=True,
                )

        self.assertEqual(0, result)
        self.assertFalse(tidy_invoked)
        self.assertIn("no changed C/C++ translation units", output.getvalue())
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: FAIL — `run_workflow()` raises `TypeError: run_workflow() got an unexpected keyword argument 'changed'`.

- [ ] **Step 3: Implement the `changed` parameter and CLI flag**

In `run_workflow`, change the signature from:

```python
def run_workflow(
    *,
    mode: str,
    workspace: Path,
    compdb_tool: str,
    platform_name: str,
    environ: Mapping[str, str],
    command_runner: CommandRunner = subprocess.run,
    build_args: Sequence[str] = (),
    compdb_args: Sequence[str] = (),
) -> int:
```

to:

```python
def run_workflow(
    *,
    mode: str,
    workspace: Path,
    compdb_tool: str,
    platform_name: str,
    environ: Mapping[str, str],
    command_runner: CommandRunner = subprocess.run,
    build_args: Sequence[str] = (),
    compdb_args: Sequence[str] = (),
    changed: bool = False,
) -> int:
```

Change:

```python
    entries = load_project_entries(workspace, workspace / "compile_commands.json")
    tools = discover_tools(mode, platform_name=platform_name, environ=environ)
```

to:

```python
    entries = load_project_entries(workspace, workspace / "compile_commands.json")
    if changed:
        changed_paths = changed_files(workspace, command_runner)
        entries, notes = filter_changed_entries(entries, changed_paths, workspace.resolve())
        for note in notes:
            print(note)
        if not entries:
            print("clang-tidy: no changed C/C++ translation units to analyze, skipping.")
            return 0
    tools = discover_tools(mode, platform_name=platform_name, environ=environ)
```

In `main`, change:

```python
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("report", "fix"))
    parser.add_argument("--compdb-tool", required=True)
```

to:

```python
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("report", "fix"))
    parser.add_argument("--changed", action="store_true")
    parser.add_argument("--compdb-tool", required=True)
```

and change:

```python
        return run_workflow(
            mode=args.mode,
            workspace=root,
            compdb_tool=str(tool),
            platform_name=sys.platform,
            environ=os.environ,
            build_args=args.build_args,
            compdb_args=args.compdb_args,
        )
```

to:

```python
        return run_workflow(
            mode=args.mode,
            workspace=root,
            compdb_tool=str(tool),
            platform_name=sys.platform,
            environ=os.environ,
            build_args=args.build_args,
            compdb_args=args.compdb_args,
            changed=args.changed,
        )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: PASS — all tests, including the two new ones. This also runs the complete existing suite; confirm no regression before moving on.

- [ ] **Step 5: Commit**

```bash
git add scripts/clang_tidy_runner.py scripts/clang_tidy_runner_test.py
git commit -m "feat(clang-tidy): add --changed CLI flag to scope analysis to a git diff"
```

---

## Task 6: New Bazel targets for the changed-files gate

**Files:**
- Modify: `BUILD.bazel`

**Interfaces:**
- Consumes: `--changed` CLI flag from Task 5.
- Produces: Bazel targets `//:clang_tidy_fix_changed`, `//:clang_tidy_report_changed`.

- [ ] **Step 1: Add the two targets**

In `BUILD.bazel`, immediately after the existing `clang_tidy_fix` target (which ends just before the file's final blank line), add:

```python
py_binary(
    name = "clang_tidy_fix_changed",
    srcs = ["scripts/clang_tidy_runner.py"],
    args = [
        "fix",
        "--changed",
        "--compdb-tool",
        "$(location //bazel/compile_commands:refresh)",
        "--build-arg=--config=release",
        "--build-arg=//...",
        "--compdb-arg=--config=release",
    ],
    data = [
        ".clang-tidy",
        "//bazel/compile_commands:refresh",
    ],
    main = "scripts/clang_tidy_runner.py",
    deps = ["@python_deps//pyyaml"],
)

py_binary(
    name = "clang_tidy_report_changed",
    srcs = ["scripts/clang_tidy_runner.py"],
    args = [
        "report",
        "--changed",
        "--compdb-tool",
        "$(location //bazel/compile_commands:refresh)",
        "--build-arg=--config=release",
        "--build-arg=//...",
        "--compdb-arg=--config=release",
    ],
    data = [
        ".clang-tidy",
        "//bazel/compile_commands:refresh",
    ],
    main = "scripts/clang_tidy_runner.py",
    deps = ["@python_deps//pyyaml"],
)
```

- [ ] **Step 2: Verify the targets build**

Run: `bazel build --config=release //:clang_tidy_fix_changed //:clang_tidy_report_changed //:clang_tidy_runner_test`
Expected: all three build successfully (this only builds the `py_binary`/`py_test` wrappers — it does not execute clang-tidy).

Run: `bazel test --config=release //:clang_tidy_runner_test --test_output=all`
Expected: PASS — unaffected by the BUILD file addition.

- [ ] **Step 3: Commit**

```bash
git add BUILD.bazel
git commit -m "build: add clang_tidy_fix_changed and clang_tidy_report_changed targets"
```

---

## Task 7: Point pr.yml at the changed-files report

**Files:**
- Modify: `.github/workflows/pr.yml`

**Interfaces:**
- Consumes: `//:clang_tidy_report_changed` (Task 6).

- [ ] **Step 1: Add `fetch-depth: 0` to the `bazel` job's checkout**

In `.github/workflows/pr.yml`, in the `bazel` job (the one with `strategy.matrix.os: [windows-latest, macos-latest, ubuntu-26.04]`), change:

```yaml
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1

      - uses: bazel-contrib/setup-bazel@c5acdfb288317d0b5c0bbd7a396a3dc868bb0f86 # 0.19.0
```

to:

```yaml
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          fetch-depth: 0

      - uses: bazel-contrib/setup-bazel@c5acdfb288317d0b5c0bbd7a396a3dc868bb0f86 # 0.19.0
```

This mirrors the pattern `release.yml` already uses and makes `origin/master` reachable so `git merge-base HEAD origin/master` (Task 3) succeeds in CI.

- [ ] **Step 2: Swap the clang-tidy step to the changed-files target**

Change:

```yaml
      - name: clang-tidy report
        run: |
          bazel run --config=release //:clang_tidy_report
```

to:

```yaml
      - name: clang-tidy report
        run: |
          bazel run --config=release //:clang_tidy_report_changed
```

- [ ] **Step 3: Validate the YAML**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/pr.yml'))"`
Expected: no output, exit code 0 (confirms the file still parses; there is no workflow linter in this repo's `prek` hooks). Real behavioral verification happens when this branch's PR runs its own `pr.yml` — the `pull_request` trigger uses the workflow file version from the PR branch itself.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/pr.yml
git commit -m "ci: gate PRs on the changed-files clang-tidy report"
```

---

## Task 8: Correct the tech-debt doc's static-analysis section

**Files:**
- Modify: `docs/tech-debt.md`

The existing "P2: Turn static analysis into a ratchet" section describes `pr.yml`'s report step as `continue-on-error: true` (no longer accurate — it's blocking) and assumes a full-repo report still runs on every PR. After Task 7, `pr.yml` only ever checks changed files; the actual remaining gap is that a pre-existing violation in code nobody touches now has **no CI signal at all**, since the full-repo report no longer runs anywhere automatically.

- [ ] **Step 1: Rewrite the section**

In `docs/tech-debt.md`, replace the "P2: Turn static analysis into a ratchet" section's body:

```markdown
The Bazel-driven clang-tidy report and autofix commands are useful and covered
by runner tests, but the PR workflow marks the report step
`continue-on-error: true`. New diagnostics can therefore accumulate without a
failing signal.

Actions:

- Record a machine-readable baseline or allowlist by check and source path.
- Fail CI on new diagnostics in changed maintained code, then reduce the
  baseline by ownership area.
- Keep generated Qt code, vendored code, Bazel outputs, and external headers out
  of the baseline.
- Promote the report to a required CI result after the ratchet is deterministic
  across Linux, macOS, and Windows compilation commands.
```

with:

```markdown
`pr.yml` gates every PR on `//:clang_tidy_report_changed` (changed files
only, `WarningsAsErrors: '*'`, blocking, per OS). Full-repo
`clang_tidy_report`/`clang_tidy_fix` remain available as manual targets but
no longer run anywhere in CI, so a pre-existing violation in code a PR
doesn't touch has no CI signal at all.

Actions:

- Record a machine-readable baseline or allowlist by check and source path
  for the full-repo report.
- Add a scheduled or release-triggered job that runs the full-repo report
  against that baseline and fails only on new diagnostics outside it, then
  reduce the baseline by ownership area over time.
- Keep generated Qt code, vendored code, Bazel outputs, and external headers
  out of the baseline.
- Promote that job to a required, blocking result once the ratchet is
  deterministic across Linux, macOS, and Windows compilation commands.
```

- [ ] **Step 2: Run the doc lint hook**

Run: `prek run --files docs/tech-debt.md`
Expected: PASS (trailing-whitespace/end-of-file/lychee link checks all clean — no links were added).

- [ ] **Step 3: Commit**

```bash
git add docs/tech-debt.md
git commit -m "docs: correct tech-debt P2 for the changed-files PR gate"
```
