#!/usr/bin/env python3
"""Enforce ownership and runner policy for centralized C++ tests."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CPP_SUFFIXES = {".cc", ".cpp", ".cxx"}
MAIN_RE = re.compile(r"\b(?:int|auto)\s+main\s*\(")
GTEST_RE = re.compile(r"#\s*include\s*[<\"]gtest/|\bTEST(?:_F|_P)?\s*\(")
BOILERPLATE_RE = re.compile(r"InitGoogleTest|RUN_ALL_TESTS")


def load_allowlist(path: Path) -> tuple[dict[str, str], list[str]]:
    entries: dict[str, str] = {}
    errors: list[str] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        item, separator, rationale = line.partition("|")
        if not separator or not rationale.strip():
            errors.append(f"{path}:{number}: allowlist entry is missing a rationale")
            continue
        if item in entries:
            errors.append(f"{path}:{number}: duplicate allowlist entry: {item}")
        entries[item] = rationale.strip()
    return entries, errors


def check(root: Path, allowlist_path: Path) -> list[str]:
    allowlist, errors = load_allowlist(allowlist_path)
    test_root = root / "tests"
    actual = {
        path.relative_to(root).as_posix()
        for path in test_root.rglob("*")
        if path.is_file() and path.suffix in CPP_SUFFIXES
    }
    for path in sorted(actual - allowlist.keys()):
        errors.append(f"{path}: centralized C++ source is not allowlisted with a rationale")
    for path in sorted(allowlist.keys() - actual):
        errors.append(f"{path}: stale allowlist entry")

    for relative in sorted(actual):
        text = (root / relative).read_text(encoding="utf-8", errors="replace")
        if GTEST_RE.search(text):
            errors.append(f"{relative}: GoogleTest suites must be package-owned")
        if BOILERPLATE_RE.search(text):
            errors.append(f"{relative}: GoogleTest runner boilerplate is forbidden")
        if MAIN_RE.search(text) and relative not in allowlist:
            errors.append(f"{relative}: handwritten main requires an allowlisted rationale")

    package_sources_seen = 0
    for source_root in (root / "apps", root / "resources", root / "src", root / "tests"):
        if not source_root.is_dir():
            continue
        for path in source_root.rglob("*"):
            if not path.is_file() or path.suffix not in CPP_SUFFIXES:
                continue
            if source_root.name in {"apps", "resources", "src"}:
                package_sources_seen += 1
            text = path.read_text(encoding="utf-8", errors="replace")
            if BOILERPLATE_RE.search(text):
                relative = path.relative_to(root).as_posix()
                errors.append(f"{relative}: GoogleTest runner boilerplate is forbidden")
    if (root / "BUILD.bazel").is_file() and package_sources_seen == 0:
        errors.append("layout policy has no package-owned C++ sources in its runfiles")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--allowlist", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    allowlist = args.allowlist or root / "tests/layout_allowlist.txt"
    errors = check(root, allowlist)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    count = len(load_allowlist(allowlist)[0])
    print(f"OK: {count} documented integration/platform C++ sources under tests/.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
