#!/usr/bin/env python3
"""Generate and format managed packages with Gazelle and pinned prek hooks.

Exit codes: 0 = unchanged (or successfully corrected with --fix),
1 = corrections made, 2 = tool or lint failure. Snapshot all *.bazel files
before and after to catch additions, deletions and changes beyond tracked files.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tokenize
from collections.abc import Callable, Sequence
from io import StringIO
from pathlib import Path
from typing import TextIO

GAZELLE_TARGET = "//:gazelle"
BUILD_FILE_PATHSPEC = "*.bazel"
# Keep these roots aligned with GAZELLE_ARGS in the root BUILD.bazel.
MANAGED_ROOTS = tuple(
    Path(path)
    for path in (
        "src/algorithms",
        "src/backend/ports",
        "src/backend/protocol",
        "src/backend/checksum",
        "src/backend/diagnostics",
        "src/backend/config",
        "src/backend/definition",
        "src/backend/calibration",
        "src/backend/logging",
        "src/backend/service_functions",
        "src/backend/flash",
        "src/ui/desktop/calibration",
        "src/ui/desktop/checksum",
        "src/ui/desktop/menu",
        "src/ui/desktop/channels",
        "apps/bench",
        "src/platform/desktop/common/connection/testing",
        "src/platform/desktop/common/ports",
        "src/ui/desktop/definition",
        "src/platform/desktop/unix/j2534",
        "src/platform/desktop/windows/j2534",
        "src/ui/desktop/biu",
        "src/ui/desktop/hexedit",
        "src/platform/desktop/common/remote_utility",
        "tests",
        "src/platform/desktop/common/service_functions",
        "src/platform/desktop/common/transport",
        "src/platform/desktop/common/bytes",
        "src/platform/desktop/common/connection",
        "src/platform/desktop/common/testing",
        "src/platform/desktop/common/flash",
        "src/platform/desktop/common/definition",
        "src/platform/desktop/common/diagnostics",
        "src/platform/desktop/common/logging",
        "src/platform/desktop/common/serial",
        "src/platform/desktop/common/serial/testing",
        "src/ui/desktop",
        "src/ui/desktop/service_functions",
        "src/ui/desktop/flash/operation",
        "src/ui/desktop/flash/common",
        "apps/desktop",
        "tests/force_asserts",
        "resources/shared",
    )
)

ListFiles = Callable[[Path], Sequence[str]]
RunGazelle = Callable[[Path], int]
RunHook = Callable[[Path, str, Sequence[str]], int]
ShowDiff = Callable[[Path, Sequence[str], Sequence[str]], None]


class GazelleCheckError(RuntimeError):
    """An actionable failure that is not BUILD file drift."""


_CPP_TEST_RULE = re.compile(
    r"^(?:cc_test|qt_cc_test|fastecu_(?:portable_)?gtest)\s*\(", re.MULTILINE
)


_CPP_PRODUCTION_RULE = re.compile(
    r"^(?P<kind>cc_library|qt_cc_library|cc_binary|qt_cc_binary)\s*\(", re.MULTILINE
)
_RULE_NAME = re.compile(r"\bname\s*=\s*\"([^\"]+)\"")

# Whole-rule keeps on C++ libraries and binaries that remain hand-owned. The
# list only shrinks: migrate a rule to generation, then delete its entry. A
# narrow attribute or dependency keep needs no entry.
KEPT_CPP_PRODUCTION_RULES: frozenset[str] = frozenset(
    {
        "src/platform/desktop/common/ports:qt_event_sink",
        "src/platform/desktop/unix/j2534:j2534",
        "src/platform/desktop/windows/j2534:pe_bitness_x64_fixture",
        "src/ui/desktop/definition:definition_authoring_dialog",
        "tests:fake_j2534_dll_native",
    }
)

_KEEP_COMMENT = re.compile(r"#\s*keep(?:: .*)?\s*$")


def _rule_suffix_is_kept(source: str) -> bool:
    """Find the rule's closing parenthesis without confusing nested expressions."""
    depth = 0
    for token in tokenize.generate_tokens(StringIO(source).readline):
        if token.type != tokenize.OP:
            continue
        if token.string == "(":
            depth += 1
        elif token.string == ")":
            depth -= 1
            if depth == 0:
                row, column = token.end
                suffix = source.splitlines()[row - 1][column:].strip()
                return bool(_KEEP_COMMENT.fullmatch(suffix))
    return False


def validate_test_ownership(root: Path, files: Sequence[str]) -> None:
    """All C++ test packages regenerate; keeps may preserve individual attributes only."""
    for relative in files:
        path = root / relative
        if path.suffix not in {".bazel", ".bzl"} or not path.is_file():
            continue
        source = path.read_text()
        for match in _CPP_TEST_RULE.finditer(source):
            if not any(Path(relative).is_relative_to(managed) for managed in MANAGED_ROOTS):
                raise GazelleCheckError(f"{relative}: C++ test package outside Gazelle scope")
            if _rule_suffix_is_kept(source[match.start() :]):
                raise GazelleCheckError(f"{relative}: C++ test has a whole-rule keep")
            preceding = source[: match.start()].splitlines()
            for line in reversed(preceding):
                if line.strip() and not line.lstrip().startswith("#"):
                    break
                if _KEEP_COMMENT.fullmatch(line.strip()):
                    raise GazelleCheckError(f"{relative}: C++ test has a whole-rule keep")


def _has_whole_rule_keep(source: str, match: re.Match[str]) -> bool:
    if _rule_suffix_is_kept(source[match.start() :]):
        return True
    for line in reversed(source[: match.start()].splitlines()):
        if line.strip() and not line.lstrip().startswith("#"):
            return False
        if _KEEP_COMMENT.fullmatch(line.strip()):
            return True
    return False


def kept_production_rules(root: Path, files: Sequence[str]) -> set[str]:
    """Labels (`dir:name`) of C++ libraries and binaries carrying a whole-rule keep."""
    kept: set[str] = set()
    for relative in files:
        path = root / relative
        if path.name != "BUILD.bazel" or not path.is_file():
            continue
        source = path.read_text()
        for match in _CPP_PRODUCTION_RULE.finditer(source):
            if not _has_whole_rule_keep(source, match):
                continue
            name = _RULE_NAME.search(source, match.end())
            label = name.group(1) if name else "<unnamed>"
            kept.add(f"{Path(relative).parent.as_posix()}:{label}")
    return kept


def validate_production_keeps(
    root: Path,
    files: Sequence[str],
    allowed: frozenset[str] | None = None,
) -> None:
    """Reject whole-rule keeps on C++ libraries and binaries outside the shrinking allowlist.

    Entries whose rule no longer carries a keep are stale and rejected too, so the
    list shrinks as rules migrate to generation.
    """
    allowed = KEPT_CPP_PRODUCTION_RULES if allowed is None else allowed
    kept = kept_production_rules(root, files)
    for label in sorted(kept - allowed):
        raise GazelleCheckError(
            f"{label}: C++ library or binary has a whole-rule keep outside the "
            "KEPT_CPP_PRODUCTION_RULES allowlist; use a narrow attribute or dependency keep"
        )
    for label in sorted(allowed - kept):
        raise GazelleCheckError(
            f"{label}: stale KEPT_CPP_PRODUCTION_RULES entry; the rule no longer has a "
            "whole-rule keep, so remove the entry"
        )


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
    # Same options as the documented build commands, so a push does not make the
    # Bazel server discard its analysis cache over a changed configuration.
    return subprocess.run(
        [bazel, "run", "--config=release", GAZELLE_TARGET], cwd=root, check=False
    ).returncode


def managed_build_files(root: Path, list_files: ListFiles) -> list[str]:
    """Discover managed BUILD files after generation, including new ones."""
    return sorted(
        relative
        for relative in list_files(root)
        if Path(relative).name == "BUILD.bazel"
        and any(Path(relative).is_relative_to(managed) for managed in MANAGED_ROOTS)
        and (root / relative).is_file()
    )


def run_prek_hook(root: Path, hook: str, files: Sequence[str]) -> int:
    prek = shutil.which("prek")
    if prek is None:
        raise GazelleCheckError("prek was not found on PATH")
    return subprocess.run([prek, "run", hook, "--files", *files], cwd=root, check=False).returncode


def git_show_diff(root: Path, paths: Sequence[str], added: Sequence[str]) -> None:
    """Print the change without touching the index; created files are diffed against nothing."""
    existing = [path for path in paths if path not in added]
    if existing:
        subprocess.run(["git", "--no-pager", "diff", "--", *existing], cwd=root, check=False)
    for path in added:
        subprocess.run(
            ["git", "--no-pager", "diff", "--no-index", "--", "/dev/null", path],
            cwd=root,
            check=False,
        )


def check(
    root: Path,
    list_files: ListFiles,
    run_gazelle: RunGazelle,
    show_diff: ShowDiff,
    out: TextIO,
    *,
    run_hook: RunHook = run_prek_hook,
    fix: bool = False,
    allowed_keeps: frozenset[str] | None = None,
) -> int:
    validate_test_ownership(root, list_files(root))
    validate_production_keeps(root, list_files(root), allowed_keeps)
    before = snapshot(root, list_files)
    exit_code = run_gazelle(root)
    if exit_code != 0:
        print(f"gazelle failed (exit {exit_code}); BUILD files were not checked.", file=out)
        return 2
    files = managed_build_files(root, list_files)
    if files:
        pre_format = snapshot(root, lambda _root: files)
        exit_code = run_hook(root, "buildifier", files)
        # prek returns 1 for successful corrections too. Confirm a correcting
        # formatter succeeds on a second pass; an unchanged failure is an error.
        if exit_code == 1 and pre_format != snapshot(root, lambda _root: files):
            exit_code = run_hook(root, "buildifier", files)
        if exit_code != 0:
            print(f"buildifier failed (exit {exit_code}); BUILD files were not checked.", file=out)
            return 2
        exit_code = run_hook(root, "buildifier-lint", files)
        if exit_code != 0:
            print(
                f"buildifier-lint failed (exit {exit_code}); BUILD files were not checked.",
                file=out,
            )
            return 2
    after = snapshot(root, list_files)
    paths = changed_paths(before, after)
    if not paths:
        return 0
    print("Gazelle/Buildifier changed these BUILD files:", file=out)
    for path in paths:
        print(f"  {path}", file=out)
    show_diff(root, paths, [path for path in paths if path not in before])
    print(
        "Review the changes above, then `git add` and commit them before pushing again: "
        "a re-push with the fixes only staged passes this check but sends the stale files.",
        file=out,
    )
    return 0 if fix else 1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fix", action="store_true", help="apply corrections and return 0 on success"
    )
    args = parser.parse_args(argv)
    try:
        root = repo_root()
        validate_test_ownership(
            root, [str(path.relative_to(root)) for path in (root / "bazel").rglob("*.bzl")]
        )
        return check(
            root,
            git_list_build_files,
            run_bazel_gazelle,
            git_show_diff,
            sys.stderr,
            fix=args.fix,
        )
    except (GazelleCheckError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
