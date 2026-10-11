"""Summarize the report `run-clang-tidy -enable-check-profile` prints."""

from __future__ import annotations

import re
from collections.abc import Mapping

# A table row is N columns of `<seconds> (<percent>%)` followed by the check
# name. The columns are user, system, user+system, wall and, on newer LLVM,
# an instruction count; wall time is always the fourth.
# The lookbehind stops the scan restarting inside a digit run, which made a
# long run of digits quadratic.
_COLUMN = re.compile(r"(?<![\d.])(\d+(?:\.\d+)?(?:e[+-]?\d+)?) {1,20}\( {0,20}\d+(?:\.\d+)?%\)")
_WALL_COLUMN = 3


def parse_check_profile(text: str) -> dict[str, float]:
    """Wall seconds per check, summed over every table found in `text`.

    run-clang-tidy aggregates one table on recent LLVM but may print one per
    file on older releases; summing by name handles both. The `Total` row is
    dropped.
    """
    totals: dict[str, float] = {}
    for line in text.splitlines():
        columns = _COLUMN.findall(line)
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
