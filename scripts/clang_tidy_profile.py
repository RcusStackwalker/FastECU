"""Summarize the report `run-clang-tidy -enable-check-profile` prints."""

from __future__ import annotations

from collections.abc import Mapping
from itertools import pairwise

# A table row is N columns of `<seconds> (<percent>%)` followed by the check
# name. The columns are user, system, user+system, wall and, on newer LLVM,
# an instruction count; wall time is always the fourth.
_WALL_COLUMN = 3


def _is_number(token: str) -> bool:
    if not token[:1].isdigit():
        return False
    try:
        float(token)
    except ValueError:
        return False
    return True


def _columns(line: str) -> list[str]:
    """The `<value>` of every `<value> (<percent>%)` pair in `line`, in order.

    Tokenised rather than matched with a regex: a column pattern scanned
    across a long digit run backtracks super-linearly.
    """
    # Split `(` off what precedes it and close up any padding inside it.
    tokens = " ".join(line.replace("(", " (").split()).replace("( ", "(").split()
    return [
        value
        for value, share in pairwise(tokens)
        if _is_number(value)
        and share.startswith("(")
        and share.endswith("%)")
        and _is_number(share[1:-2])
    ]


def parse_check_profile(text: str) -> dict[str, float]:
    """Wall seconds per check, summed over every table found in `text`.

    run-clang-tidy aggregates one table on recent LLVM but may print one per
    file on older releases; summing by name handles both. The `Total` row is
    dropped.
    """
    totals: dict[str, float] = {}
    for line in text.splitlines():
        columns = _columns(line)
        if len(columns) <= _WALL_COLUMN:
            continue
        name = line.rsplit(None, 1)[-1]
        if name == "Total" or name.endswith(")"):
            continue
        totals[name] = totals.get(name, 0.0) + float(columns[_WALL_COLUMN])
    return totals


def render_markdown(
    checks: Mapping[str, float],
    phases: Mapping[str, float],
    translation_units: int,
    top: int = 15,
) -> str:
    lines = [
        "### clang-tidy profile",
        "",
        f"Translation units: {translation_units}",
        "",
        "| Phase | Seconds |",
        "|---|---:|",
    ]
    lines.extend(f"| {name} | {seconds:.1f} |" for name, seconds in phases.items())
    lines.append("")
    if not checks:
        lines.append("No check profile data was found in the clang-tidy output.")
        lines.append("")
        return "\n".join(lines)
    total = sum(checks.values())
    lines.extend(
        [
            "| Check | Seconds (summed over files) | Share |",
            "|---|---:|---:|",
        ]
    )
    slowest = sorted(checks.items(), key=lambda item: item[1], reverse=True)[:top]
    for name, seconds in slowest:
        share = 100.0 * seconds / total if total else 0.0
        lines.append(f"| {name} | {seconds:.1f} | {share:.1f}% |")
    lines.append("")
    return "\n".join(lines)
