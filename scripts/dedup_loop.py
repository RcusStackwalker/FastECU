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
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import TextIO
from urllib.error import URLError
from urllib.parse import urlencode
from urllib.request import urlopen

import mutation_test

SCOPE = ("src/", "apps/", "tests/")
PRODUCTION = "production"
TEST = "test"
RunHook = Callable[[Path], str]
SONAR = "https://sonarcloud.io"
PROJECT = "RcusStackwalker_FastECU"
Fetch = Callable[[str, Mapping[str, str]], Mapping]
FILE_SCOPE = "<file>"
_PAGE_SIZE = 500
LEDGER = Path("docs/reference/sonar-dedup-ledger.json")
TREND = Path("docs/reference/dedup-trend.csv")
TREND_HEADER = ("date", "pr", "nloc_before", "nloc_after", "dup_before", "dup_after")
OUTCOMES = ("open", "merged", "closed", "failed", "unsuitable", "deferred")
# Judged by reading: these cost no implementation, so they neither count toward
# nor break a run of failures.
_NEUTRAL = ("open", "unsuitable", "deferred")
# A loop PR's group is settled; it is not offered again.
_SETTLED = ("open", "merged", "unsuitable", "deferred")
MIN_DROP = 10
RunGit = Callable[[Path, Sequence[str]], str]
BusyFiles = Callable[[Path], set[str]]

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


@dataclass(frozen=True)
class Attempt:
    group: str
    members: list[list[str]]
    outcome: str
    date: str
    reason: str = ""
    pr: int | None = None

    def __post_init__(self) -> None:
        if self.outcome not in OUTCOMES:
            raise DedupLoopError(f"unknown outcome {self.outcome!r}; expected one of {OUTCOMES}")


@dataclass
class Ledger:
    """Every judgment and attempt, committed so a fresh clone sees them."""

    attempts: list[Attempt] = field(default_factory=list)

    @classmethod
    def load(cls, path: Path) -> Ledger:
        if not path.exists():
            return cls()
        try:
            return cls([Attempt(**a) for a in json.loads(path.read_text())["attempts"]])
        except (ValueError, KeyError, TypeError) as error:
            raise DedupLoopError(f"unreadable ledger {path}: {error}") from error

    def save(self, path: Path) -> None:
        data = {"attempts": [asdict(a) for a in self.attempts]}
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")

    def record(self, attempt: Attempt) -> None:
        self.attempts.append(attempt)

    def outcome(self, group: str) -> str | None:
        return next((a.outcome for a in reversed(self.attempts) if a.group == group), None)

    def consecutive_failures(self) -> int:
        count = 0
        for attempt in reversed(self.attempts):
            if attempt.outcome == "failed":
                count += 1
            elif attempt.outcome not in _NEUTRAL:
                break
        return count


@dataclass(frozen=True)
class TrendRow:
    date: str
    pr: int
    nloc_before: int
    nloc_after: int
    dup_before: int | None
    dup_after: int | None


def append_trend(path: Path, row: TrendRow, *, bookkeeping: bool = False) -> None:
    if not bookkeeping and row.nloc_before - row.nloc_after < MIN_DROP:
        raise DedupLoopError(
            f"NLOC {row.nloc_before} -> {row.nloc_after}: "
            f"a loop PR lowers it by at least {MIN_DROP}"
        )
    new = not path.exists()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        if new:
            writer.writerow(TREND_HEADER)
        writer.writerow(["" if v is None else v for v in asdict(row).values()])


def fill_dup_after(path: Path, value: int) -> None:
    """Record master's duplicated lines after the last row's merge."""
    rows = list(csv.reader(path.read_text().splitlines())) if path.exists() else []
    if len(rows) < 2 or rows[-1][-1]:
        raise DedupLoopError(f"no trend row in {path} awaits dup_after")
    rows[-1][-1] = str(value)
    with path.open("w", newline="") as handle:
        csv.writer(handle, lineterminator="\n").writerows(rows)


def master_duplicated_lines(fetch: Fetch) -> int:
    response = fetch(
        "api/measures/component", {"component": PROJECT, "metricKeys": "duplicated_lines"}
    )
    return int(response["component"]["measures"][0]["value"])


def analysis_revision(fetch: Fetch) -> str:
    response = fetch("api/project_analyses/search", {"project": PROJECT, "ps": "1"})
    return response["analyses"][0]["revision"]


def open_non_loop_pr_files(root: Path) -> set[str]:
    """Files touched by open PRs that are not loop PRs, so the loop stays out of their way."""
    prs = json.loads(
        _run_tool(
            root,
            ["gh", "pr", "list", "--state", "open", "--limit", "100", "--json", "files,labels"],
        )
    )
    return {
        f["path"]
        for pr in prs
        if "sonar-dedup" not in {label["name"] for label in pr["labels"]}
        for f in pr["files"]
    }


def run_git(root: Path, args: Sequence[str]) -> str:
    return _run_tool(root, ["git", *args])


def rank(groups: Sequence[Group], ledger: Ledger, busy: set[str]) -> list[Group]:
    """The queue: backend first, then most duplicated lines; settled or busy groups dropped."""
    open_groups = [
        g
        for g in groups
        if ledger.outcome(g.id) not in _SETTLED and not any(b.path in busy for b in g.blocks)
    ]
    return sorted(
        open_groups,
        key=lambda g: (
            not all(b.path.startswith("src/backend/") for b in g.blocks),
            -g.lines,
            g.id,
        ),
    )


PR_METRICS = ("new_duplicated_lines", "new_uncovered_lines")


def pr_measures(fetch: Fetch, pr: int) -> dict[str, int]:
    """A PR analysis's new-code metrics; SonarCloud omits a metric with nothing to report."""
    params = {"component": PROJECT, "pullRequest": str(pr), "metricKeys": ",".join(PR_METRICS)}
    found = {
        m["metric"]: m for m in fetch("api/measures/component", params)["component"]["measures"]
    }
    result = {}
    for metric in PR_METRICS:
        measure_ = found.get(metric, {})
        value = measure_.get("value") or next(
            (p.get("value") for p in measure_.get("periods", [])), "0"
        )
        result[metric] = int(float(value))
    return result


def survivors(directory: Path | None, root: Path) -> list[str]:
    """Surviving mutants in the `mutation` job's reports, which hold changed lines only."""
    if directory is None or not directory.is_dir():
        return []
    merged = mutation_test.merge_reports(mutation_test.read_reports(directory), root)
    return [
        f"{k.path}:{k.line}:{k.column} {k.mutator}"
        for k, status in sorted(merged.items())
        if status == "Survived"
    ]


def is_bookkeeping(changed: Sequence[str]) -> bool:
    return bool(changed) and set(changed) <= {LEDGER.as_posix(), TREND.as_posix()}


@dataclass(frozen=True)
class Check:
    name: str
    ok: bool
    detail: str


def verdict(
    *,
    nloc_before: int,
    nloc_after: int,
    changed: Sequence[str],
    measures: Mapping[str, int],
    survived: Sequence[str],
) -> list[Check]:
    """The merge rules of a loop PR (ADR 0029)."""
    drop = nloc_before - nloc_after
    duplicated = measures.get("new_duplicated_lines", 0)
    uncovered = measures.get("new_uncovered_lines", 0)
    shown = "; ".join(survived[:10]) + (
        f"; ... {len(survived) - 10} more" if len(survived) > 10 else ""
    )
    return [
        Check(
            "nloc drop",
            drop >= MIN_DROP or is_bookkeeping(changed),
            f"{nloc_before} -> {nloc_after} (at least {MIN_DROP} required)",
        ),
        Check("new duplicated lines", duplicated == 0, str(duplicated)),
        Check("new uncovered lines", uncovered == 0, str(uncovered)),
        Check("surviving mutants", not survived, shown or "0"),
    ]


def _line_count(root: Path) -> Callable[[str], int | None]:
    def count(path: str) -> int | None:
        file = root / path
        return len(file.read_text(errors="replace").splitlines()) if file.is_file() else None

    return count


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

    commands.add_parser("candidates", help="print the ranked duplication-group queue")
    commands.add_parser("fresh", help="exit 0 when master's analysis is of origin/master")

    ledger_parser = commands.add_parser("ledger", help="record or list judgments and attempts")
    ledger_commands = ledger_parser.add_subparsers(dest="ledger_command", required=True)
    record = ledger_commands.add_parser("record")
    record.add_argument("group")
    record.add_argument("outcome", choices=OUTCOMES)
    record.add_argument("--members-json", default="[]", help='[["path", "function"], ...]')
    record.add_argument("--reason", default="")
    record.add_argument("--pr", type=int)
    ledger_commands.add_parser("list")

    trend = commands.add_parser("trend", help="keep the NLOC and duplication trend")
    trend_commands = trend.add_subparsers(dest="trend_command", required=True)
    add = trend_commands.add_parser("add", help="append a row, measuring NLOC after")
    add.add_argument("--pr", type=int, required=True)
    add.add_argument("--before", type=int, required=True, help="NLOC of the base")
    add.add_argument("--dup-before", type=int, required=True, help="master duplicated_lines")
    add.add_argument("--bookkeeping", action="store_true", help="exempt from the NLOC rule")
    trend_commands.add_parser("fill-after", help="record master's duplicated_lines now")

    check = commands.add_parser("verdict", help="check a loop PR against its merge rules")
    check.add_argument("--pr", type=int, required=True)
    check.add_argument("--nloc-before", type=int, required=True)
    check.add_argument("--nloc-after", type=int, required=True)
    check.add_argument("--changed-file", type=Path, required=True, help="git diff --name-only")
    check.add_argument("--mutation-reports", type=Path, help="the mutation job's report dir")
    return parser.parse_args(argv)


@dataclass(frozen=True)
class Tools:
    """The external tools each command runs; tests substitute them."""

    run_hook: RunHook
    fetch: Fetch
    run_functions: RunHook
    busy_files: BusyFiles
    git: RunGit
    err: TextIO


def _run(args: argparse.Namespace, tools: Tools, out: TextIO, today: str) -> int:
    root = args.root
    if args.command == "measure":
        result = measure(root, tools.run_hook)
        if args.json:
            out.write(json.dumps({"total": result.total, **asdict(result)}) + "\n")
        else:
            out.write(f"total {result.total}\nproduction {result.production}\ntest {result.test}\n")
    elif args.command == "candidates":
        ranges = parse_function_ranges(tools.run_functions(root))
        groups = build_groups(tools.fetch, ranges, _line_count(root), err=tools.err)
        for group in rank(groups, Ledger.load(root / LEDGER), tools.busy_files(root)):
            out.write(f"{group.id} lines={group.lines}\n")
            for block in group.blocks:
                names = ",".join(name for _path, name in key_block(block, ranges))
                out.write(f"  {block.path}:{block.start}-{block.end} {names}\n")
    elif args.command == "fresh":
        analysed = analysis_revision(tools.fetch)
        head = tools.git(root, ["rev-parse", "origin/master"]).strip()
        if analysed != head:
            out.write(f"stale: analysis {analysed}, origin/master {head}\n")
            return 1
        out.write(f"fresh {head}\n")
    elif args.command == "ledger":
        ledger = Ledger.load(root / LEDGER)
        if args.ledger_command == "record":
            members = json.loads(args.members_json)
            ledger.record(Attempt(args.group, members, args.outcome, today, args.reason, args.pr))
            ledger.save(root / LEDGER)
        else:
            for a in ledger.attempts:
                fields = (a.date, a.group, a.outcome, a.reason, a.pr or "")
                out.write(" ".join(str(v) for v in fields if v) + "\n")
            out.write(f"consecutive failures: {ledger.consecutive_failures()}\n")
    elif args.command == "verdict":
        checks = verdict(
            nloc_before=args.nloc_before,
            nloc_after=args.nloc_after,
            changed=args.changed_file.read_text().split(),
            measures=pr_measures(tools.fetch, args.pr),
            survived=survivors(args.mutation_reports, root),
        )
        for c in checks:
            out.write(f"{'PASS' if c.ok else 'FAIL'} {c.name}: {c.detail}\n")
        return 0 if all(c.ok for c in checks) else 1
    elif args.trend_command == "add":
        after = measure(root, tools.run_hook).total
        row = TrendRow(today, args.pr, args.before, after, args.dup_before, None)
        append_trend(root / TREND, row, bookkeeping=args.bookkeeping)
    else:
        fill_dup_after(root / TREND, master_duplicated_lines(tools.fetch))
    return 0


def main(
    argv: Sequence[str] | None = None,
    run_hook: RunHook = run_nloc_hook,
    out: TextIO = sys.stdout,
    today: Callable[[], str] = lambda: datetime.date.today().isoformat(),
    *,
    fetch: Fetch = sonar_get,
    run_functions: RunHook = run_function_hook,
    busy_files: BusyFiles = open_non_loop_pr_files,
    git: RunGit = run_git,
    err: TextIO = sys.stderr,
) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        tools = Tools(run_hook, fetch, run_functions, busy_files, git, err)
        return _run(args, tools, out, today())
    except (DedupLoopError, mutation_test.MutationTestError, OSError) as error:
        out.write(f"dedup_loop: {error}\n")
        return 2


if __name__ == "__main__":
    sys.exit(main())
