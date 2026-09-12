#!/usr/bin/env python3
"""Reject Qt widget code anywhere under src/backend.

Backend code must never construct or show a user interface -- see
docs/superpowers/specs/2026-09-01-step6a-file-actions-dewidget-design.md.
The one accepted match is Q_OBJECT inside a *_test.cpp file's own QtTest
fixture class: QtTest requires Q_OBJECT for its moc-based test-slot
mechanism, which has nothing to do with widgets.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

from python.runfiles import Runfiles

ANCHOR_RLOCATION = "src/backend/definitions/file_actions.h"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
FORBIDDEN = re.compile(r"QMessageBox|QFileDialog|QDialog|QWidget|Q_OBJECT")
MIN_EXPECTED_FILES = 100


def _find_backend_root() -> Path:
    """Locate the real (not runfiles-copied) src/backend directory.

    The scan needs the whole src/backend tree, and Bazel can only put files
    in a test's runfiles if they are declared: src/backend spans 19 packages
    and glob() does not cross package boundaries, so declaring the tree means
    19 per-package filegroups that a newly added package silently drops out
    of -- exactly the blind spot this guard exists to close. So instead it
    anchors on file_actions.h, a real `data` dependency, resolves it through
    the runfiles API, and walks the directory that anchor lives in.

    That works only where runfiles entries are symlinks back into the real
    checkout. On Windows they are not: `--enable_runfiles` (needed so Qt's
    plugin loader can see real plugin files, see .bazelrc) makes Bazel
    materialize the tree by *copying*, so the anchor resolves to the copy and
    its parent holds that one file rather than the source tree. Nothing
    reachable from a sandboxed test points back out at the checkout there,
    which is why //:backend_no_widgets is marked incompatible with Windows in
    BUILD.bazel; Linux and macOS run it on every PR and the verdict is a
    property of the source text, not of the host.

    MIN_EXPECTED_FILES below is the backstop: if this ever resolves to a
    partial tree again, the check fails loudly instead of passing vacuously.
    """
    runfiles = Runfiles.Create()
    if runfiles is not None:
        workspace = os.environ.get("TEST_WORKSPACE", "_main")
        anchor = runfiles.Rlocation(f"{workspace}/{ANCHOR_RLOCATION}")
        if anchor and Path(anchor).is_file():
            return Path(anchor).resolve().parents[1]
    # Not running under `bazel test` (e.g. invoked directly for local iteration).
    return Path(__file__).resolve().parents[1] / "src" / "backend"


BACKEND = _find_backend_root()
ROOT = BACKEND.parent.parent


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
    scanned = 0
    for path in sorted(BACKEND.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        scanned += 1
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

    if scanned < MIN_EXPECTED_FILES:
        errors.append(
            f"backend_no_widgets scanned only {scanned} files under {BACKEND} "
            f"(expected at least {MIN_EXPECTED_FILES}) -- the scan is probably "
            "running against an incomplete runfiles tree rather than the real "
            "source tree; this check cannot prove anything if it can't see the code"
        )

    if errors:
        print("\n".join(errors))
        print()
        print("src/backend must never construct or show a user interface.")
        print("Move UI code to src/ui or src/platform, or route through IEventSink.")
        return 1
    print(f"OK: no forbidden widget code found under src/backend ({scanned} files scanned).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
