"""Convert Bazel's combined lcov report to Sonar generic coverage XML."""

import argparse
import re
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

IGNORE = re.compile(
    r"(^|/)(tests|hexedit)/|(^|/)(moc_|qrc_|ui_)|\.moc$|rep_.*_replica\.h"
    r"|(^|/)Qt/[0-9][^/]*/|/Applications/|/opt/homebrew/|/Library/Developer/|bazel-out/|external/"
)


@dataclass(frozen=True)
class FileCoverage:
    lines: dict[int, int]  # line -> hits
    branches: dict[int, tuple[int, int]]  # line -> (to_cover, covered)


def parse_lcov(text: str) -> dict[str, FileCoverage]:
    lines: dict[str, dict[int, int]] = {}
    taken: dict[str, dict[tuple[int, str, str], int]] = {}
    current = None
    for raw in text.splitlines():
        tag, _, value = raw.strip().partition(":")
        if tag == "SF":
            current = value
            lines.setdefault(current, {})
            taken.setdefault(current, {})
        elif current is None:
            continue
        elif tag == "DA":
            number, hits, *_ = value.split(",")
            lines[current][int(number)] = lines[current].get(int(number), 0) + int(hits)
        elif tag == "BRDA":
            number, block, branch, count = value.split(",")
            key = (int(number), block, branch)
            taken[current][key] = taken[current].get(key, 0) + (0 if count == "-" else int(count))
        elif tag == "end_of_record":
            current = None
    records = {}
    for path, hits in lines.items():
        branches: dict[int, tuple[int, int]] = {}
        for (number, _, _), count in taken[path].items():
            total, covered = branches.get(number, (0, 0))
            branches[number] = (total + 1, covered + (count > 0))
        records[path] = FileCoverage(hits, branches)
    return records


def select_files(
    records: dict[str, FileCoverage], root: Path, ignore: re.Pattern[str]
) -> dict[str, FileCoverage]:
    return {
        path: cov
        for path, cov in records.items()
        if not ignore.search(path)
        and not path.startswith("/")
        and not Path(path).is_absolute()
        and ".." not in Path(path).parts
        and (root / path).is_file()
    }


def generic_xml(records: dict[str, FileCoverage]) -> str:
    out = ['<coverage version="1">']
    for path in sorted(records):
        cov = records[path]
        out.append(f'  <file path="{_escape(path)}">')
        for number in sorted(cov.lines):
            attrs = f'lineNumber="{number}" covered="{"true" if cov.lines[number] else "false"}"'
            if number in cov.branches:
                total, covered = cov.branches[number]
                attrs += f' branchesToCover="{total}" coveredBranches="{covered}"'
            out.append(f"    <lineToCover {attrs}/>")
        out.append("  </file>")
    out.append("</coverage>")
    return "\n".join(out) + "\n"


def _escape(value: str) -> str:
    return (
        value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")
    )


def summary(records: dict[str, FileCoverage]) -> str:
    out = []
    covered_total = line_total = 0
    for path in sorted(records):
        hits = records[path].lines.values()
        covered = sum(1 for h in hits if h)
        out.append(f"{path} {covered}/{len(hits)}")
        covered_total += covered
        line_total += len(hits)
    pct = 100.0 * covered_total / line_total if line_total else 0.0
    out.append(f"TOTAL {covered_total}/{line_total} {pct:.1f}%")
    return "\n".join(out) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lcov", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--min-files", type=int, default=100)
    args = parser.parse_args(argv)

    records = parse_lcov(args.lcov.read_text(errors="replace"))
    kept = select_files(records, args.root, IGNORE)
    if len(kept) < args.min_files:
        dropped = sorted(set(records) - set(kept))[:5]
        print(
            f"only {len(kept)} of {len(records)} lcov files resolved under {args.root} "
            f"(minimum {args.min_files}); first dropped: {', '.join(dropped) or 'none'}",
            file=sys.stderr,
        )
        return 1
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "sonar-coverage.xml").write_text(generic_xml(kept))
    (args.output_dir / "coverage-summary.txt").write_text(summary(kept))
    return 0


if __name__ == "__main__":
    sys.exit(main())
