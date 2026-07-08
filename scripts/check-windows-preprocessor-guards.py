#!/usr/bin/env python3
"""Reject bare WIN32 preprocessor guards in project sources.

MSVC defines _WIN32, but Bazel does not necessarily define WIN32. A bare WIN32
guard can make Windows builds take POSIX branches such as <unistd.h>.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
SKIP_DIRS = {".claude", ".git", "bazel-bin", "bazel-out", "bazel-testlogs"}
BARE_WIN32 = re.compile(r"^\s*#\s*(?:ifn?def|if|elif)\b.*\bWIN32\b")
ALLOWED = ("_WIN32", "Q_OS_WIN32")


def main() -> int:
    errors: list[str] = []
    for path in sorted(ROOT.rglob("*")):
        if any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts):
            continue
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        for lineno, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
            if BARE_WIN32.search(line) and not any(token in line for token in ALLOWED):
                errors.append(
                    f"{path.relative_to(ROOT)}:{lineno}: use _WIN32 or Q_OS_WIN32 instead of bare WIN32"
                )

    if errors:
        print("\n".join(errors))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
