#!/usr/bin/env python3
"""Verify Markdown references to repository files resolve to something real.

Two reference shapes are checked, because this repository uses both:

- inline links, ``[text](docs/tech-debt.md)``
- backticked paths, ``see `docs/tech-debt.md` `` -- the dominant idiom here,
  and the one that silently rotted when the step-5 design docs moved

Only in-repository targets are checked. External URLs are skipped on purpose:
a pre-commit hook must not need the network (ADR 0002).
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Fenced code blocks hold C++ that looks like link syntax to a regex --
# `](const DirEntry& e)` from a lambda capture is not a broken link.
FENCE = re.compile(r"^\s*(```|~~~)")

INLINE_LINK = re.compile(r"(?<!!)\[[^\]\n]*\]\(([^)\s]+)\)")
BACKTICKED_DOC = re.compile(r"`([^`\s]+\.md)`")

SKIP_PREFIXES = ("http://", "https://", "mailto:", "#")

# Deliberate references to files in sibling repositories of the parent
# workspace. Each is correct where it is written and unresolvable from inside
# this repository. Add an entry only when the surrounding prose already says
# the target lives elsewhere -- a bare unresolvable path is a bug, not an
# external reference.
EXTERNAL_REFS = {
    # README's DMA wake sequence, spelled out in the parent research repo.
    "docs/superpowers/specs/2026-06-07-oem-kline-dma-activation-and-wire-protocol.md",
    # The mode-23 bench checklist that the Colt CZT CAN gate mirrors.
    "mmc-patches/m32r/39670016/z27a_mt_audm/mode23-bench-notes.md",
}


def strip_code_blocks(text: str) -> list[tuple[int, str]]:
    """Return (line number, text) pairs outside fenced code blocks."""
    lines: list[tuple[int, str]] = []
    in_fence = False
    for number, line in enumerate(text.splitlines(), start=1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if not in_fence:
            lines.append((number, line))
    return lines


def targets(line: str) -> list[str]:
    found = [m.group(1) for m in INLINE_LINK.finditer(line)]
    found += [m.group(1) for m in BACKTICKED_DOC.finditer(line)]
    return [t for t in found if not t.startswith(SKIP_PREFIXES) and t not in EXTERNAL_REFS]


def resolves(source: Path, target: str) -> bool:
    """A target may be written relative to its own file or to the repo root."""
    path = target.split("#", 1)[0].split("?", 1)[0]
    if not path:
        return True
    candidates = [source.parent / path, ROOT / path.lstrip("/")]
    return any(c.exists() for c in candidates)


def display(path: Path) -> str:
    """Repo-relative where possible; argv may name a file outside the tree."""
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def tracked_markdown() -> list[Path]:
    out = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z", "*.md"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return [ROOT / name for name in out.split("\0") if name]


def main(argv: list[str]) -> int:
    paths = [Path(a).resolve() for a in argv] if argv else tracked_markdown()
    errors: list[str] = []
    checked = 0

    for path in sorted(paths):
        if path.suffix != ".md" or not path.is_file():
            continue
        for number, line in strip_code_blocks(path.read_text()):
            for target in targets(line):
                checked += 1
                if not resolves(path, target):
                    errors.append(f"{display(path)}:{number}: broken link -> {target}")

    if checked == 0 and not argv:
        errors.append("markdown link check found no references to verify")

    if errors:
        print("\n".join(errors))
        print(f"\n{len(errors)} broken reference(s). Fix the path or drop the reference.")
        return 1
    print(f"OK: {checked} in-repository markdown references resolve.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
