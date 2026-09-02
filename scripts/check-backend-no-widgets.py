#!/usr/bin/env python3
"""Reject Qt widget code anywhere under src/backend.

Backend code must never construct or show a user interface -- see
docs/superpowers/specs/2026-09-01-step6a-file-actions-dewidget-design.md.
The one accepted match is Q_OBJECT inside a *_test.cpp file's own QtTest
fixture class: QtTest requires Q_OBJECT for its moc-based test-slot
mechanism, which has nothing to do with widgets.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / "src" / "backend"
SOURCE_SUFFIXES = {".h", ".cpp"}
FORBIDDEN = re.compile(r"QMessageBox|QFileDialog|QDialog|QWidget|Q_OBJECT")


def strip_comments(line: str, in_block_comment: bool) -> tuple[str, bool]:
    """Return (line with comments blanked out, still-in-block-comment)."""
    code_chars: list[str] = []
    i = 0
    n = len(line)
    while i < n:
        if in_block_comment:
            end = line.find("*/", i)
            if end == -1:
                return "".join(code_chars), True
            in_block_comment = False
            i = end + 2
            continue
        two = line[i : i + 2]
        if two == "//":
            break
        if two == "/*":
            end = line.find("*/", i + 2)
            if end == -1:
                in_block_comment = True
                break
            i = end + 2
            continue
        code_chars.append(line[i])
        i += 1
    return "".join(code_chars), in_block_comment


def main() -> int:
    errors: list[str] = []
    for path in sorted(BACKEND.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        in_block_comment = False
        for lineno, line in enumerate(path.read_text(errors="ignore").splitlines(), 1):
            code, in_block_comment = strip_comments(line, in_block_comment)
            for match in FORBIDDEN.finditer(code):
                if match.group(0) == "Q_OBJECT" and path.name.endswith("_test.cpp"):
                    continue
                errors.append(
                    f"{path.relative_to(ROOT)}:{lineno}: "
                    f"{match.group(0)} is not allowed under src/backend"
                )

    if errors:
        print("\n".join(errors))
        print()
        print("src/backend must never construct or show a user interface.")
        print("Move UI code to src/ui or src/platform, or route through IEventSink.")
        return 1
    print("OK: no forbidden widget code found under src/backend.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
