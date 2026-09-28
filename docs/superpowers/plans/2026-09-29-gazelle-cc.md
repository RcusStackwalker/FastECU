# gazelle_cc Adoption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put the BUILD files under `src/algorithms` (minus `qt_compat`) under `gazelle_cc` management, enforced by a separate CI job and a local pre-push hook.

**Architecture:** `//:gazelle` (gazelle 0.54.0 + gazelle_cc 0.6.0, `cc_group unit`, `map_kind` to `fastecu_portable_gtest`) is scoped by its `args`. `scripts/gazelle_check.py` snapshots every `*.bazel` file, runs `bazel run //:gazelle`, and fails if any file changed, was created, or was deleted. One prek `pre-push` hook calls the script; a `gazelle` job in `pr.yml` runs that same hook.

**Tech Stack:** Bazel 9.1.1 (bzlmod), gazelle, gazelle_cc, Python 3.14 (standard library only), prek 0.4.0, GitHub Actions.

**Spec:** [docs/superpowers/specs/2026-09-29-gazelle-cc-design.md](../specs/2026-09-29-gazelle-cc-design.md)

## Global Constraints

- `MODULE.bazel`: `bazel_dep(name = "gazelle", version = "0.54.0", dev_dependency = True)` and `bazel_dep(name = "gazelle_cc", version = "0.6.0", dev_dependency = True)`.
- Grouping mode is `# gazelle:cc_group unit`; `# gazelle:cc_group directory` is rejected.
- Scope is the `args` list of the `gazelle` rule (`src/algorithms`); `# gazelle:exclude src/algorithms/protocol/qt_compat`.
- Gazelle never writes `visibility`; new packages need a hand-written `package(default_visibility = ...)`.
- `PORTABLE_PACKAGES` and the `qt_layer` package group are never edited by gazelle.
- The script uses the standard library only, and does not depend on bash tools (Windows contributors).
- prek does not notice files a hook creates, so the script must detect created and deleted BUILD files itself.
- The `@googletest//:gtest` dependency gazelle adds to tests is redundant with `gtest_main` and is accepted as-is.
- Do not commit on `master`. Do not push or open a PR without the user's authorization.
- Commit messages end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Python: ruff (`line-length = 100`, `select = ["E", "F", "UP", "B", "SIM", "I"]`) and unittest, co-located, wired with `py_test` in the root `BUILD.bazel` like `clang_tidy_runner_test`.

## Review Focus

Failure modes the spec implies but a naive implementation would miss, most likely first. Each has a test in the owning task.

- **Unrelated dirty BUILD edits.** A contributor with an unstaged edit to some `BUILD.bazel` must not be told gazelle drifted when gazelle changed nothing. Expected: exit 0 (Task 1).
- **Gazelle itself fails** (bazel error, unresolved dependency with `cc_unresolved_deps error`). Expected: a distinct non-zero exit and a message saying the check did not run, never "clean" (Task 1).
- **Gazelle creates a BUILD file.** Expected: exit 1 and the new file named; prek alone would pass (Task 1, Task 4).
- **Gazelle deletes a BUILD file.** Expected: exit 1 (Task 1).
- **Paths with spaces or a missing `bazel`.** Expected: paths parsed via NUL separators; a missing `bazel` gives an actionable message and exit 2, not a traceback (Task 1).
- **Generated test repeats an implicit dependency.** `fastecu_portable_gtest` adds `byte_matchers` itself and gazelle lists it too; Bazel rejects a duplicated label. Expected: the macro tolerates it (Task 2).
- **`implementation_deps` rewrites break a downstream target** that was silently relying on a transitive header. Expected: caught by building and testing the whole tree before the baseline is committed (Task 3).

---

## File Structure

| Path | Responsibility |
|---|---|
| `scripts/gazelle_check.py` (create) | Snapshot BUILD files, run gazelle, report drift. |
| `scripts/gazelle_check_test.py` (create) | Unit tests for the above. |
| `BUILD.bazel` (modify) | `py_test` for the script; `gazelle_binary`, `gazelle`, directives. |
| `MODULE.bazel` (modify) | `gazelle` and `gazelle_cc` dev dependencies. `MODULE.bazel.lock` follows. |
| `bazel/gtest_targets.bzl` (modify) | Deduplicate `deps` in `fastecu_portable_gtest`. |
| `src/algorithms/**/BUILD.bazel` (modify) | Gazelle baseline. |
| `.pre-commit-config.yaml` (modify) | `gazelle` pre-push hook. |
| `.github/workflows/pr.yml` (modify) | `gazelle` job. |
| `docs/adr/0017-manage-build-files-with-gazelle.md` (create), `docs/adr/README.md`, `CLAUDE.md` (modify) | Decision record and pointer. |

Work happens on the existing branch `docs/gazelle-cc-design`, which already holds the spec.

---

### Task 1: `gazelle_check.py` with tests

**Files:**
- Create: `scripts/gazelle_check.py`
- Create: `scripts/gazelle_check_test.py`
- Modify: `BUILD.bazel` (add `py_test` after `clang_tidy_runner_test`)

**Interfaces:**
- Produces (used by Task 4's hook and Task 5's CI):
  - `scripts/gazelle_check.py` executable, exit `0` = clean, `1` = drift, `2` = the check could not run.
  - `check(root: Path, list_files: ListFiles, run_gazelle: RunGazelle, show_diff: ShowDiff, out: TextIO) -> int`
  - `git_list_build_files(root: Path) -> list[str]`, `run_bazel_gazelle(root: Path) -> int`, `git_show_diff(root: Path, paths: Sequence[str], added: Sequence[str]) -> None`, `main() -> int`, `class GazelleCheckError(RuntimeError)`.

- [ ] **Step 1: Write the failing tests**

Create `scripts/gazelle_check_test.py`:

```python
#!/usr/bin/env python3

import shutil
import subprocess
import tempfile
import unittest
from io import StringIO
from pathlib import Path
from unittest import mock

import gazelle_check as gc

_BUILD = "BUILD.bazel"


def list_all(root: Path) -> list[str]:
    """Stand-in for git: every *.bazel file below root, repo-relative."""
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*.bazel"))


class CheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name).resolve()
        (self.root / "pkg").mkdir()
        self.build = self.root / "pkg" / _BUILD
        self.build.write_text("# original\n")
        self.diffs: list[tuple[list[str], list[str]]] = []

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_check(self, run_gazelle) -> tuple[int, str]:
        out = StringIO()

        def show_diff(_root: Path, paths, added) -> None:
            self.diffs.append((list(paths), list(added)))

        code = gc.check(self.root, list_all, run_gazelle, show_diff, out)
        return code, out.getvalue()

    def test_unchanged_tree_is_clean(self) -> None:
        code, output = self.run_check(lambda _root: 0)
        self.assertEqual(code, 0)
        self.assertEqual(output, "")
        self.assertEqual(self.diffs, [])

    def test_modified_file_is_drift(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.write_text("# regenerated\n")
            return 0

        code, output = self.run_check(gazelle)
        self.assertEqual(code, 1)
        self.assertIn("pkg/BUILD.bazel", output)
        self.assertEqual(self.diffs, [(["pkg/BUILD.bazel"], [])])

    def test_created_file_is_drift_and_reported_as_added(self) -> None:
        def gazelle(root: Path) -> int:
            (root / "new").mkdir()
            (root / "new" / _BUILD).write_text("# created\n")
            return 0

        code, output = self.run_check(gazelle)
        self.assertEqual(code, 1)
        self.assertIn("new/BUILD.bazel", output)
        self.assertEqual(self.diffs, [(["new/BUILD.bazel"], ["new/BUILD.bazel"])])

    def test_deleted_file_is_drift(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.unlink()
            return 0

        code, output = self.run_check(gazelle)
        self.assertEqual(code, 1)
        self.assertIn("pkg/BUILD.bazel", output)

    def test_preexisting_uncommitted_edit_is_not_drift(self) -> None:
        self.build.write_text("# contributor edit, not yet staged\n")
        code, _ = self.run_check(lambda _root: 0)
        self.assertEqual(code, 0)

    def test_gazelle_failure_is_not_reported_as_clean(self) -> None:
        code, output = self.run_check(lambda _root: 1)
        self.assertEqual(code, 2)
        self.assertIn("gazelle failed", output)
        self.assertEqual(self.diffs, [])

    def test_gazelle_failure_after_partial_write_still_exits_two(self) -> None:
        def gazelle(_root: Path) -> int:
            self.build.write_text("# half written\n")
            return 1

        code, _ = self.run_check(gazelle)
        self.assertEqual(code, 2)

    def test_snapshot_ignores_listed_but_missing_files(self) -> None:
        snapshot = gc.snapshot(self.root, lambda _root: ["pkg/BUILD.bazel", "gone/BUILD.bazel"])
        self.assertEqual(list(snapshot), ["pkg/BUILD.bazel"])


class EnvironmentTest(unittest.TestCase):
    def test_missing_bazel_is_an_actionable_error(self) -> None:
        with (
            mock.patch.object(gc.shutil, "which", return_value=None),
            self.assertRaisesRegex(gc.GazelleCheckError, "bazel"),
        ):
            gc.run_bazel_gazelle(Path("."))

    def test_main_reports_environment_errors_as_exit_two(self) -> None:
        with (
            mock.patch.object(gc, "repo_root", side_effect=gc.GazelleCheckError("no repo")),
            mock.patch.object(gc.sys, "stderr", new=StringIO()) as stderr,
        ):
            self.assertEqual(gc.main(), 2)
        self.assertIn("no repo", stderr.getvalue())


@unittest.skipUnless(shutil.which("git"), "git is required")
class GitListBuildFilesTest(unittest.TestCase):
    def test_lists_tracked_and_untracked_but_not_ignored_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            (root / ".gitignore").write_text("ignored/\n")
            for name in ("tracked", "dir with space", "ignored"):
                (root / name).mkdir()
                (root / name / _BUILD).write_text("#\n")
            (root / "README.md").write_text("not a build file\n")
            subprocess.run(["git", "add", "tracked"], cwd=root, check=True)

            self.assertEqual(
                gc.git_list_build_files(root),
                ["dir with space/BUILD.bazel", "tracked/BUILD.bazel"],
            )


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd scripts && python3 gazelle_check_test.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'gazelle_check'`.

- [ ] **Step 3: Write the implementation**

Create `scripts/gazelle_check.py` (then `chmod +x scripts/gazelle_check.py`):

```python
#!/usr/bin/env python3
"""Regenerate BUILD files with gazelle and fail if any *.bazel file changed.

Exit codes: 0 = BUILD files are up to date, 1 = gazelle changed, created or
deleted a file, 2 = the check could not run (no repository, no bazel, or
gazelle itself failed).

prek only notices tracked files that a hook modifies, so this script compares
its own before/after snapshot and also catches created and deleted files.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import TextIO

GAZELLE_TARGET = "//:gazelle"
BUILD_FILE_PATHSPEC = "*.bazel"

ListFiles = Callable[[Path], Sequence[str]]
RunGazelle = Callable[[Path], int]
ShowDiff = Callable[[Path, Sequence[str], Sequence[str]], None]


class GazelleCheckError(RuntimeError):
    """An actionable failure that is not BUILD file drift."""


def repo_root() -> Path:
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], check=False, capture_output=True, text=True
    )
    if result.returncode != 0:
        raise GazelleCheckError("not inside a git repository")
    return Path(result.stdout.strip())


def git_list_build_files(root: Path) -> list[str]:
    """Tracked and untracked (non-ignored) *.bazel files, repo-relative and sorted."""
    result = subprocess.run(
        [
            "git",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            BUILD_FILE_PATHSPEC,
        ],
        cwd=root,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip()
        raise GazelleCheckError(f"git ls-files failed: {detail}")
    return sorted({path for path in result.stdout.decode().split("\0") if path})


def snapshot(root: Path, list_files: ListFiles) -> dict[str, str]:
    """Content digest of every listed file that exists on disk."""
    digests: dict[str, str] = {}
    for relative in list_files(root):
        path = root / relative
        if path.is_file():
            digests[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return digests


def changed_paths(before: dict[str, str], after: dict[str, str]) -> list[str]:
    return sorted(path for path in before.keys() | after.keys() if before.get(path) != after.get(path))


def run_bazel_gazelle(root: Path) -> int:
    bazel = shutil.which("bazel") or shutil.which("bazelisk")
    if bazel is None:
        raise GazelleCheckError("bazel (or bazelisk) was not found on PATH")
    return subprocess.run([bazel, "run", GAZELLE_TARGET], cwd=root, check=False).returncode


def git_show_diff(root: Path, paths: Sequence[str], added: Sequence[str]) -> None:
    """Print the change; created files are marked intent-to-add so `git diff` shows them."""
    if added:
        subprocess.run(["git", "add", "--intent-to-add", "--", *added], cwd=root, check=False)
    subprocess.run(["git", "--no-pager", "diff", "--", *paths], cwd=root, check=False)


def check(
    root: Path,
    list_files: ListFiles,
    run_gazelle: RunGazelle,
    show_diff: ShowDiff,
    out: TextIO,
) -> int:
    before = snapshot(root, list_files)
    exit_code = run_gazelle(root)
    if exit_code != 0:
        print(f"gazelle failed (exit {exit_code}); BUILD files were not checked.", file=out)
        return 2
    after = snapshot(root, list_files)
    paths = changed_paths(before, after)
    if not paths:
        return 0
    print("gazelle changed these BUILD files:", file=out)
    for path in paths:
        print(f"  {path}", file=out)
    show_diff(root, paths, [path for path in paths if path not in before])
    print("Review the changes above, `git add` them, and push again.", file=out)
    return 1


def main() -> int:
    try:
        return check(
            repo_root(), git_list_build_files, run_bazel_gazelle, git_show_diff, sys.stderr
        )
    except GazelleCheckError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd scripts && python3 gazelle_check_test.py`
Expected: `Ran 11 tests ... OK` (the git test is skipped only if `git` is missing).

- [ ] **Step 5: Add the Bazel test target**

In `BUILD.bazel`, directly after the `clang_tidy_runner_test` target, add:

```python
py_test(
    name = "gazelle_check_test",
    size = "small",
    srcs = [
        "scripts/gazelle_check.py",
        "scripts/gazelle_check_test.py",
    ],
    imports = ["scripts"],
    main = "scripts/gazelle_check_test.py",
)
```

Run: `bazel test --config=release //:gazelle_check_test`
Expected: `PASSED`.

- [ ] **Step 6: Lint and commit**

```bash
prek run --files scripts/gazelle_check.py scripts/gazelle_check_test.py BUILD.bazel
git add scripts/gazelle_check.py scripts/gazelle_check_test.py BUILD.bazel
git commit -m "build: Add gazelle_check.py to detect BUILD file drift

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```
Expected: all hooks pass. If ruff reformats a file, `git add` it again and re-run the commit.

---

### Task 2: Wire gazelle and make the test macro tolerate repeated deps

**Files:**
- Modify: `MODULE.bazel` (after the `pugixml` dependency, line 8)
- Modify: `BUILD.bazel` (top of file: loads, target, directives)
- Modify: `bazel/gtest_targets.bzl` (`_fastecu_portable_gtest_impl`)

**Interfaces:**
- Produces: `bazel run //:gazelle` regenerates `src/algorithms/**/BUILD.bazel` (consumed by Tasks 3 and 4); `fastecu_portable_gtest` accepts a `deps` list that repeats `//src/algorithms/protocol/testing:byte_matchers`.

- [ ] **Step 1: Reproduce the duplicate-label failure first**

Temporarily add the implicit dependency to one test so it mirrors what gazelle will produce. In `src/algorithms/checksum/BUILD.bazel`, change the `checksum_primitives_test` target to:

```python
fastecu_portable_gtest(
    name = "checksum_primitives_test",
    srcs = ["checksum_primitives_test.cpp"],
    deps = [
        ":checksum",
        "//src/algorithms/protocol/testing:byte_matchers",
    ],
)
```

Run: `bazel build --config=release //src/algorithms/checksum:checksum_primitives_test`
Expected: FAIL with an error naming `byte_matchers` as duplicated in the `deps` attribute.

- [ ] **Step 2: Deduplicate in the macro**

In `bazel/gtest_targets.bzl`, replace the `deps = [...] + deps,` expression in `_fastecu_portable_gtest_impl` with:

```python
        deps = _unique([
            Label("//src/algorithms/protocol/testing:byte_matchers"),
            Label("@googletest//:gtest_main"),
        ] + deps),
```

and add above the implementation function:

```python
def _unique(labels):
    """Order-preserving de-duplication; gazelle lists deps the macro also adds."""
    seen = {}
    for label in labels:
        seen[str(label)] = label
    return list(seen.values())
```

Note: `deps` reaches a symbolic macro's implementation as `Label` objects, so the implicit entries must be `Label(...)` too. Plain strings would never compare equal to the caller's labels and the duplicate would survive.

- [ ] **Step 3: Verify the duplicate now builds, then revert the scratch edit**

Run: `bazel build --config=release //src/algorithms/checksum:checksum_primitives_test`
Expected: PASS.

Then `git checkout src/algorithms/checksum/BUILD.bazel` (the real change comes from gazelle in Task 3).

- [ ] **Step 4: Add the dependencies**

In `MODULE.bazel`, after `bazel_dep(name = "pugixml", version = "1.15")`:

```python
bazel_dep(name = "gazelle", version = "0.54.0", dev_dependency = True)
bazel_dep(name = "gazelle_cc", version = "0.6.0", dev_dependency = True)
```

- [ ] **Step 5: Add the gazelle target and directives**

In the root `BUILD.bazel`, add this load with the other loads at the top:

```python
load("@gazelle//:def.bzl", "gazelle", "gazelle_binary")
```

and this block after the `package(...)` line:

```python
# Scope is the `args` list: widening gazelle to another area means adding a path.
# gazelle:cc_group unit
# gazelle:map_kind cc_test fastecu_portable_gtest //bazel:gtest_targets.bzl
# gazelle:exclude src/algorithms/protocol/qt_compat
gazelle_binary(
    name = "gazelle_cc_binary",
    languages = ["@gazelle_cc//language/cc"],
)

gazelle(
    name = "gazelle",
    args = ["src/algorithms"],
    gazelle = ":gazelle_cc_binary",
)
```

- [ ] **Step 6: Verify it resolves and runs in diff mode**

Run: `bazel run //:gazelle -- -mode=diff`
Expected: builds, prints a unified diff limited to files under `src/algorithms/` (nothing under `qt_compat`), exits 0. The `-mode=diff` argument is appended after `args`; if gazelle rejects that combination, run `bazel run //:gazelle -- -mode=diff src/algorithms` instead and record which form worked in the ADR (Task 6).

- [ ] **Step 7: Commit**

```bash
prek run --files MODULE.bazel BUILD.bazel bazel/gtest_targets.bzl
git add MODULE.bazel MODULE.bazel.lock BUILD.bazel bazel/gtest_targets.bzl
git commit -m "build: Add gazelle_cc and dedupe fastecu_portable_gtest deps

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Generate and verify the baseline

**Files:**
- Modify: `src/algorithms/**/BUILD.bazel` (as generated; expect about 10 files)

**Interfaces:**
- Consumes: `//:gazelle` from Task 2.
- Produces: a tree on which `scripts/gazelle_check.py` exits 0 (required by Tasks 4 and 5).

- [ ] **Step 1: Run gazelle**

Run: `bazel run //:gazelle`
Expected: exit 0; `git status --short` lists only `BUILD.bazel` files under `src/algorithms/`.

- [ ] **Step 2: Confirm the change classes match the spec**

Run: `git diff -U0 -- 'src/algorithms/**/BUILD.bazel' | grep -E '^[-+][^-+]' | sort | uniq -c | sort -rn`

Every line must belong to one of the spec's classes: added `"@googletest//:gtest",`; added `"//src/algorithms/protocol/testing:byte_matchers",`; `implementation_deps` introduced; `//pkg` to `:pkg` normalisation; the handful of removed or rewritten `//src/algorithms/protocol` and `//src/algorithms/protocol/uds` deps. Also run `git diff --stat -- src/algorithms/protocol/qt_compat`, expected empty.

If a rule is added or removed, or a library is renamed, stop: that is a class the spec does not cover. Do not commit; report the diff to the user.

- [ ] **Step 3: Check formatting**

Run: `prek run buildifier buildifier-lint --files $(git diff --name-only)`
Expected: pass. If buildifier reformats, that means gazelle and buildifier disagree; re-run gazelle afterwards and confirm Step 5 (idempotence) still holds. If they fight, report it.

- [ ] **Step 4: Build and test the whole tree**

```bash
bazel build -k --config=release //...
bazel test -k --config=release //...
```
Expected: both pass. `implementation_deps` can hide a header from targets that included it transitively; a failure shows an undeclared-inclusion or missing-header error naming the downstream target. Fix by adding the direct dependency to that target by hand (packages outside `src/algorithms` are not gazelle-managed), then re-run both commands. A build-graph guard failure (`//:portable_closure`) is not to be worked around: report it.

- [ ] **Step 5: Confirm idempotence**

```bash
before=$(git diff | shasum)
bazel run //:gazelle
after=$(git diff | shasum)
[ "$before" = "$after" ] && echo IDEMPOTENT
```
Expected: prints `IDEMPOTENT`.

- [ ] **Step 6: Check the script against the baseline**

Run: `python3 scripts/gazelle_check.py; echo "exit=$?"`
Expected: no output, `exit=0` (the script runs gazelle a second time and finds nothing to change).

- [ ] **Step 7: Commit**

```bash
git add src/algorithms
git commit -m "build: Apply the gazelle baseline to src/algorithms

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 4: prek pre-push hook

**Files:**
- Modify: `.pre-commit-config.yaml` (append to the `repo: local` hooks, after `no-using-namespace-in-headers`)

**Interfaces:**
- Consumes: `scripts/gazelle_check.py` (Task 1), clean baseline (Task 3).
- Produces: hook id `gazelle`, stage `pre-push` (Task 5 invokes it by id).

- [ ] **Step 1: Add the hook**

Append inside the existing `repo: local` block:

```yaml
      - id: gazelle
        name: BUILD files match gazelle output
        language: python
        entry: scripts/gazelle_check.py
        pass_filenames: false
        stages: [pre-push]
        files: '(\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$)|((^|/)BUILD\.bazel$)|(^MODULE\.bazel$)|(^scripts/gazelle_check\.py$)'
```

- [ ] **Step 2: Verify the clean case**

Run: `prek run gazelle --hook-stage pre-push --all-files`
Expected: `BUILD files match gazelle output......Passed`.

If prek cannot build the hook environment with `language: python`, switch to `language: system` with `entry: python3 scripts/gazelle_check.py` and re-run.

- [ ] **Step 3: Verify that created files are gated**

```bash
mkdir src/algorithms/scratch_gazelle
printf '#pragma once\nint scratch();\n' > src/algorithms/scratch_gazelle/scratch.h
printf '#include "scratch.h"\nint scratch() { return 0; }\n' > src/algorithms/scratch_gazelle/scratch.cpp
prek run gazelle --hook-stage pre-push --all-files; echo "exit=$?"
```
Expected: `Failed`, output naming `src/algorithms/scratch_gazelle/BUILD.bazel` and the generated `cc_library`, `exit=1`.

- [ ] **Step 4: Verify that an edit is gated, then clean up**

```bash
rm -rf src/algorithms/scratch_gazelle
git checkout -- src/algorithms
git status --short
```
Expected: `git status` shows only `.pre-commit-config.yaml`.

Then confirm the tracked-file path. Remove one dependency line that gazelle owns and run the hook:

```bash
python3 - <<'EOF'
from pathlib import Path

path = Path("src/algorithms/checksum/BUILD.bazel")
text = path.read_text()
needle = '        "@googletest//:gtest",\n'
assert needle in text, "baseline should contain a gtest dep"
path.write_text(text.replace(needle, "", 1))
EOF
git diff --stat -- src/algorithms/checksum/BUILD.bazel
prek run gazelle --hook-stage pre-push --all-files; echo "exit=$?"
git checkout -- src/algorithms/checksum/BUILD.bazel
```
Expected: the `git diff --stat` line shows one deleted line; the hook prints `Failed` with a diff that restores `"@googletest//:gtest",` and `exit=1`. The `git checkout` restores the file.
- [ ] **Step 5: Commit**

```bash
prek run --files .pre-commit-config.yaml
git add .pre-commit-config.yaml
git commit -m "build: Add gazelle pre-push hook

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 5: CI job

**Files:**
- Modify: `.github/workflows/pr.yml` (new job after `pre-commit`)

**Interfaces:**
- Consumes: hook id `gazelle` (Task 4).
- Produces: check named `gazelle (BUILD files)`.

- [ ] **Step 1: Add the job**

Insert after the `pre-commit` job and before `sonarcloud`. It has no `needs:`, so it runs in parallel with `pre-commit`. Action versions and pins are copied from the existing `bazel` job.

```yaml
  gazelle:
    name: gazelle (BUILD files)
    runs-on: ubuntu-26.04
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1

      - uses: bazel-contrib/setup-bazel@c5acdfb288317d0b5c0bbd7a396a3dc868bb0f86 # 0.19.0
        with:
          bazelisk-cache: true
          repository-cache: true
          disk-cache: fastecu-gazelle-${{ runner.os }}
          cache-save: false

      # Mirrors the Bazel job so module resolution sees the same host tools; drop
      # it if a run shows gazelle resolving without Qt.
      - name: Install Qt for host tools (Linux)
        uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: ${{ env.QT_VERSION }}
          modules: ${{ env.QT_MODULES }}

      - name: Install prek
        run: pipx install prek

      - name: Check BUILD files are up to date
        run: prek run gazelle --hook-stage pre-push --all-files --show-diff-on-failure
```

- [ ] **Step 2: Validate the workflow syntax**

Run: `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/pr.yml'))" && echo YAML-OK`
Expected: `YAML-OK`. If `actionlint` is installed, also run `actionlint .github/workflows/pr.yml`; expected no findings.

- [ ] **Step 3: Commit**

```bash
prek run --files .github/workflows/pr.yml
git add .github/workflows/pr.yml
git commit -m "ci: Add gazelle BUILD file check job

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

This job cannot be exercised before a push. Task 7 covers the first real run.

---

### Task 6: ADR and documentation

**Files:**
- Create: `docs/adr/0017-manage-build-files-with-gazelle.md`
- Modify: `docs/adr/README.md` (index table and the "next new ADR" line)
- Modify: `CLAUDE.md` (`Writing targets and tests` section)
- Modify: `docs/superpowers/specs/2026-09-29-gazelle-cc-design.md` (component 7)

**Interfaces:**
- Consumes: outcomes recorded during Tasks 2 to 5 (which `-mode=diff` form worked; whether `language: python` worked).

- [ ] **Step 1: Write the ADR**

Create `docs/adr/0017-manage-build-files-with-gazelle.md`:

```markdown
# ADR 0017: Manage BUILD Files with gazelle_cc

## Status

Accepted

## Context

BUILD files were maintained by hand. Adding or renaming a source, or changing an
`#include`, meant editing `srcs`, `hdrs` and `deps` in step, and drift was only
found when a build broke.

## Decision

`gazelle` with the `gazelle_cc` extension generates `cc_library` and test targets
for the areas listed in the `args` of the `//:gazelle` target in the root
`BUILD.bazel`. Currently that is `src/algorithms`, without `qt_compat`.

- Grouping is `cc_group unit`; `cc_test` is mapped to `fastecu_portable_gtest`.
- Gazelle does not write `visibility`, `PORTABLE_PACKAGES` or the `qt_layer`
  group. A new package needs a hand-written `package(default_visibility = ...)`.
- `scripts/gazelle_check.py` runs gazelle and fails if any `*.bazel` file changed,
  was created, or was deleted. It is a prek `pre-push` hook, and the `gazelle`
  job in `pr.yml` runs the same hook.
- To regenerate: `bazel run //:gazelle`, then review and `git add` the result.
- To widen: add a path to `args`, regenerate, and review the diff in its own pull
  request.

## Consequences

BUILD files under gazelle are reproducible from the sources, and CI rejects a
stale one. Gazelle adds a redundant `@googletest//:gtest` to tests
(`gtest_main`, supplied by the macro, already depends on it); this is accepted
because suppressing it would also give libraries `gtest_main`.

This is an exception to [ADR 0002](0002-use-prek-for-fast-local-checks.md), which
reserves prek for fast checks: the hook needs Bazel, so it runs at `pre-push`
only and is filtered to C++ and BUILD file changes. `--no-verify` bypasses it
locally; CI is the gate.

Other macros (`qt_cc_library`, `fastecu_gtest`) need their own `map_kind` and moc
`hdrs` / `normal_hdrs` handling before their packages can be added.
```

- [ ] **Step 2: Update the ADR index**

In `docs/adr/README.md`, add the row after the `0016` row:

```markdown
| [0017](0017-manage-build-files-with-gazelle.md) | BUILD files are generated by gazelle_cc where listed |
```

and change the line `The next new ADR is 0017.` to `The next new ADR is 0018.`

- [ ] **Step 3: Add the CLAUDE.md pointer**

In `CLAUDE.md`, under `## Writing targets and tests`, add a bullet:

```markdown
- BUILD files in gazelle-managed areas are regenerated, not hand-edited: run `bazel run //:gazelle` and commit the result. The managed areas and the reasoning are in [ADR 0017](docs/adr/0017-manage-build-files-with-gazelle.md).
```

- [ ] **Step 4: Fix the spec's documentation component**

In the spec, replace component 7 (`**Documentation.** ... coding style guide ...`) with: `**Documentation.** ADR 0017 records the decision and the regenerate/widen steps; [CLAUDE.md](../../../CLAUDE.md) points to it.`

- [ ] **Step 5: Check links and commit**

```bash
prek run --files docs/adr/0017-manage-build-files-with-gazelle.md docs/adr/README.md CLAUDE.md docs/superpowers/specs/2026-09-29-gazelle-cc-design.md
git add docs/adr CLAUDE.md docs/superpowers/specs
git commit -m "docs: Add ADR 0017 for gazelle-managed BUILD files

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```
Expected: lychee passes.

---

### Task 7: Final verification and hand-off

**Files:** none modified.

- [ ] **Step 1: Run the full local gate**

```bash
prek run --all-files
prek run gazelle --hook-stage pre-push --all-files
bazel build -k --config=release //...
bazel test -k --config=release //...
```
Expected: everything passes; `git status --short` is empty.

- [ ] **Step 2: Report to the user and stop**

Summarise commits (`git log --oneline master..HEAD`) and state the two things only CI can confirm: the `gazelle` job on ubuntu, and whether the Qt install step is needed. Also tell the user that `gazelle (BUILD files)` must be added to the required checks in branch protection by hand.

Do **not** push or open a PR. Ask the user to authorize it. When authorized: `git push -u origin docs/gazelle-cc-design`, then open one PR whose body ends with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`. If the user prefers splitting into stacked PRs, use the gh-stack workflow from memory.
