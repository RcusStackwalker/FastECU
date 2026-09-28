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
    return sorted(
        path for path in before.keys() | after.keys() if before.get(path) != after.get(path)
    )


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
