#!/usr/bin/env python3
"""Compare the findings of two clang-tidy runs, as printed by the runners.

Both the compile-database runner and the aspect runner print each finding as a header line,
`error: path:line:col [check]`. This lists the findings only one run reported.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Iterable, Sequence
from pathlib import Path

_HEADER = re.compile(
    r"^(?:\x1b\[[0-9;]*m)?(?P<severity>fatal error|error|warning)(?:\x1b\[0m)?: "
    r"(?P<location>\S+:\d+:\d+) (?:\x1b\[2m)?\[(?P<check>[^\]]+)\]"
)


def findings(lines: Iterable[str]) -> set[tuple[str, str, str]]:
    """(severity, location, check) of every finding header in `lines`."""
    found = set()
    for line in lines:
        match = _HEADER.match(line)
        if match:
            found.add((match["severity"], match["location"], match["check"]))
    return found


def compare(expected: set[tuple[str, str, str]], actual: set[tuple[str, str, str]]) -> list[str]:
    """Report lines for findings only one side has; empty when they agree."""
    lines = [f"only in expected: {' '.join(item)}" for item in sorted(expected - actual)]
    lines += [f"only in actual:   {' '.join(item)}" for item in sorted(actual - expected)]
    return lines


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("expected", type=Path, help="output of the compile-database runner")
    parser.add_argument("actual", type=Path, help="output of the aspect runner")
    args = parser.parse_args(argv)
    expected = findings(args.expected.read_text(encoding="utf-8", errors="replace").splitlines())
    actual = findings(args.actual.read_text(encoding="utf-8", errors="replace").splitlines())
    differences = compare(expected, actual)
    print(f"expected {len(expected)} findings, actual {len(actual)}")
    for line in differences:
        print(line)
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
