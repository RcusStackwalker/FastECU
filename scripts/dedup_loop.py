#!/usr/bin/env python3
"""Measure NLOC and keep the Sonar dedup loop's records.

See docs/tooling.md#sonar-dedup-loop and ADR 0029. NLOC comes from the pinned
`nloc` prek hook (Lizard). Exit codes: 0 = success, 1 = a rule failed,
2 = tool or input failure.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
from collections.abc import Callable, Mapping, Sequence
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TextIO
from urllib.error import URLError
from urllib.parse import urlencode
from urllib.request import urlopen

SCOPE = ("src/", "apps/", "tests/")
PRODUCTION = "production"
TEST = "test"
RunHook = Callable[[Path], str]
SONAR = "https://sonarcloud.io"
PROJECT = "RcusStackwalker_FastECU"
Fetch = Callable[[str, Mapping[str, str]], Mapping]
FILE_SCOPE = "<file>"
_PAGE_SIZE = 500

_FILE_HEADER = re.compile(r"^\s*NLOC\s+Avg\.NLOC\s+AvgCCN\s+Avg\.token\s+function_cnt\s+file\s*$")
_FILE_ROW = re.compile(r"^\s*(\d+)\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+\d+\s+(?:\./)?(\S.*?)\s*$")
_TOTAL_HEADER = re.compile(r"^\s*Total nloc\s")
_TOTAL_ROW = re.compile(r"^\s*(\d+)\s")
_TEST_NAME = re.compile(r"_test[._]")


class DedupLoopError(RuntimeError):
    """An actionable failure: a tool, report or input the loop cannot use."""


def parse_report(report: str) -> dict[str, int]:
    """Per-file NLOC from Lizard's file table, cross-checked against its total."""
    lines = report.splitlines()
    start = next((i for i, line in enumerate(lines) if _FILE_HEADER.match(line)), None)
    if start is None:
        raise DedupLoopError("Lizard report has no file table")
    files: dict[str, int] = {}
    for line in lines[start + 2 :]:
        row = _FILE_ROW.match(line)
        if row is None:
            break
        files[row.group(2)] = int(row.group(1))
    header = next((i for i, line in enumerate(lines) if _TOTAL_HEADER.match(line)), None)
    total = (
        _TOTAL_ROW.match(lines[header + 2])
        if header is not None and header + 2 < len(lines)
        else None
    )
    if total is None:
        raise DedupLoopError("Lizard report has no total")
    if sum(files.values()) != int(total.group(1)):
        raise DedupLoopError(
            f"Lizard file rows sum to {sum(files.values())} but its total is {total.group(1)}"
        )
    return files


def classify(path: str) -> str | None:
    """Production, test, or None when the path is outside the metric's scope."""
    if not path.startswith(SCOPE):
        return None
    parts = path.split("/")
    if parts[0] == "tests" or "testing" in parts or "testdata" in parts:
        return TEST
    return TEST if _TEST_NAME.search(parts[-1]) else PRODUCTION


@dataclass(frozen=True)
class Measurement:
    production: int
    test: int

    @property
    def total(self) -> int:
        return self.production + self.test


def measure_files(files: Mapping[str, int]) -> Measurement:
    sums = {PRODUCTION: 0, TEST: 0}
    for path, nloc in files.items():
        kind = classify(path)
        if kind is not None:
            sums[kind] += nloc
    return Measurement(production=sums[PRODUCTION], test=sums[TEST])


def run_nloc_hook(root: Path) -> str:
    return _run_tool(root, ["prek", "run", "nloc", "--hook-stage", "manual", "--all-files"])


def measure(root: Path, run_hook: RunHook = run_nloc_hook) -> Measurement:
    return measure_files(parse_report(run_hook(root)))


def sonar_get(path: str, params: Mapping[str, str], *, opener: Callable = urlopen) -> Mapping:
    """GET a SonarCloud Web API path; public projects need no token."""
    url = f"{SONAR}/{path}?{urlencode(params)}"
    try:
        with opener(url, timeout=30) as response:
            return json.load(response)
    except (URLError, OSError, ValueError) as error:
        raise DedupLoopError(f"SonarCloud {path}: {error}") from error


@dataclass(frozen=True, order=True)
class Block:
    """Duplicated lines of one file, 1-based and inclusive."""

    path: str
    start: int
    end: int


@dataclass(frozen=True)
class FunctionRange:
    path: str
    name: str
    start: int
    end: int


@dataclass(frozen=True)
class Group:
    """A duplication group, identified by the functions it occupies."""

    members: tuple[tuple[str, str], ...]
    blocks: tuple[Block, ...]
    lines: int

    @property
    def id(self) -> str:
        return hashlib.sha256(json.dumps(self.members).encode()).hexdigest()[:12]


def duplicated_files(fetch: Fetch) -> list[str]:
    """Paths of master's files with duplicated lines, across every page."""
    paths: list[str] = []
    page = 1
    while True:
        response = fetch(
            "api/measures/component_tree",
            {
                "component": PROJECT,
                "metricKeys": "duplicated_lines",
                "strategy": "leaves",
                "metricSortFilter": "withMeasuresOnly",
                "s": "metric",
                "metricSort": "duplicated_lines",
                "asc": "false",
                "ps": str(_PAGE_SIZE),
                "p": str(page),
            },
        )
        paths += [c["path"] for c in response["components"]]
        paging = response["paging"]
        if page * paging["pageSize"] >= paging["total"]:
            return paths
        page += 1


def file_groups(fetch: Fetch, path: str) -> list[tuple[Block, ...]]:
    response = fetch("api/duplications/show", {"key": f"{PROJECT}:{path}"})
    files = {ref: f["key"].removeprefix(f"{PROJECT}:") for ref, f in response["files"].items()}
    return [
        tuple(
            Block(files[b["_ref"]], b["from"], b["from"] + b["size"] - 1) for b in group["blocks"]
        )
        for group in response["duplications"]
    ]


def parse_function_ranges(output: str) -> list[FunctionRange]:
    """Function ranges from Lizard's --csv rows, skipping prek's banner."""
    ranges = []
    for row in csv.reader(line.strip() for line in output.splitlines()):
        if len(row) == 11 and row[0].isdigit():
            path = row[6].removeprefix("./")
            ranges.append(FunctionRange(path, row[7], int(row[9]), int(row[10])))
    return ranges


def run_function_hook(root: Path) -> str:
    return _run_tool(
        root, ["prek", "run", "nloc-functions", "--hook-stage", "manual", "--all-files"]
    )


def key_block(block: Block, ranges: Sequence[FunctionRange]) -> list[tuple[str, str]]:
    """The functions a block overlaps, or the file when it lies outside them all."""
    names = [
        (r.path, r.name)
        for r in ranges
        if r.path == block.path and r.start <= block.end and block.start <= r.end
    ]
    return names or [(block.path, FILE_SCOPE)]


def build_groups(
    fetch: Fetch,
    ranges: Sequence[FunctionRange],
    line_count: Callable[[str], int | None],
    *,
    err: TextIO = sys.stderr,
) -> list[Group]:
    """Production duplication groups, each once, minus blocks the checkout no longer has."""
    seen: set[frozenset[Block]] = set()
    groups = []
    for path in duplicated_files(fetch):
        for reported in file_groups(fetch, path):
            if frozenset(reported) in seen:
                continue
            seen.add(frozenset(reported))
            blocks = []
            for block in reported:
                if classify(block.path) != PRODUCTION:
                    continue
                lines = line_count(block.path)
                if lines is None or block.end > lines:
                    err.write(
                        f"dedup_loop: dropped stale block {block.path}:{block.start}-{block.end}\n"
                    )
                    continue
                blocks.append(block)
            if len(blocks) < 2:
                continue
            members = sorted({m for b in blocks for m in key_block(b, ranges)})
            size = sum(b.end - b.start + 1 for b in blocks)
            groups.append(Group(tuple(members), tuple(sorted(blocks)), size))
    return groups


def _run_tool(root: Path, command: Sequence[str]) -> str:
    tool = shutil.which(command[0])
    if tool is None:
        raise DedupLoopError(f"{command[0]} was not found on PATH")
    result = subprocess.run(
        [tool, *command[1:]], cwd=root, capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise DedupLoopError(f"{' '.join(command)} failed:\n{result.stdout}{result.stderr}")
    return result.stdout


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="repository root")
    commands = parser.add_subparsers(dest="command", required=True)

    measure_parser = commands.add_parser("measure", help="print NLOC and its split")
    measure_parser.add_argument("--json", action="store_true")
    return parser.parse_args(argv)


def _run(args: argparse.Namespace, run_hook: RunHook, out: TextIO, today: str) -> int:
    del today
    result = measure(args.root, run_hook)
    if args.json:
        out.write(json.dumps({"total": result.total, **asdict(result)}) + "\n")
    else:
        out.write(f"total {result.total}\nproduction {result.production}\ntest {result.test}\n")
    return 0


def main(
    argv: Sequence[str] | None = None,
    run_hook: RunHook = run_nloc_hook,
    out: TextIO = sys.stdout,
    today: Callable[[], str] = lambda: datetime.date.today().isoformat(),
) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return _run(args, run_hook, out, today())
    except DedupLoopError as error:
        out.write(f"dedup_loop: {error}\n")
        return 2


if __name__ == "__main__":
    sys.exit(main())
