#!/usr/bin/env python3
"""Measure NLOC, rank duplicate clusters, and keep the hill-climbing loop's records.

See docs/tooling.md#nloc-hill-climbing-loop. NLOC comes from the pinned `nloc`
prek hook (Lizard) and duplicates from the pinned `jscpd` hook. Exit codes:
0 = success, 2 = tool or input failure.
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

SCOPE = ("src/", "apps/", "tests/")
PRODUCTION = "production"
TEST = "test"
LEDGER = Path(".nloc-loop/ledger.json")
TREND = Path("docs/reference/nloc-trend.csv")
TREND_HEADER = ("date", "pr", "kind", "nloc_before", "nloc_after")
KINDS = ("test-only", "production")
OUTCOMES = ("open", "merged", "rejected", "closed")
MIN_SAVINGS = 50
JSCPD_REPORT = Path(".nloc-loop/jscpd/jscpd-report.json")
RunHook = Callable[[Path], str]
RunJscpd = Callable[[Path], Mapping]
HostFiles = Callable[[Path], set[str]]

_FILE_HEADER = re.compile(r"^\s*NLOC\s+Avg\.NLOC\s+AvgCCN\s+Avg\.token\s+function_cnt\s+file\s*$")
_FILE_ROW = re.compile(r"^\s*(\d+)\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+\d+\s+(?:\./)?(\S.*?)\s*$")
_TOTAL_HEADER = re.compile(r"^\s*Total nloc\s")
_TOTAL_ROW = re.compile(r"^\s*(\d+)\s")
_TEST_NAME = re.compile(r"(^tst_|_test[._])")


class NlocLoopError(RuntimeError):
    """An actionable failure: a tool, report or input the loop cannot use."""


def parse_report(report: str) -> dict[str, int]:
    """Per-file NLOC from Lizard's file table, cross-checked against its total."""
    lines = report.splitlines()
    start = next((i for i, line in enumerate(lines) if _FILE_HEADER.match(line)), None)
    if start is None:
        raise NlocLoopError("Lizard report has no file table")
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
        raise NlocLoopError("Lizard report has no total")
    if sum(files.values()) != int(total.group(1)):
        raise NlocLoopError(
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


@dataclass(frozen=True)
class Attempt:
    cluster: str
    outcome: str
    date: str
    reason: str = ""
    pr: int | None = None

    def __post_init__(self) -> None:
        if self.outcome not in OUTCOMES:
            raise NlocLoopError(f"unknown outcome {self.outcome!r}; expected one of {OUTCOMES}")


@dataclass
class Ledger:
    """Attempted clusters in order; a later attempt supersedes a cluster's outcome."""

    attempts: list[Attempt] = field(default_factory=list)

    @classmethod
    def load(cls, path: Path) -> Ledger:
        if not path.exists():
            return cls([])
        try:
            return cls([Attempt(**item) for item in json.loads(path.read_text())["attempts"]])
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise NlocLoopError(f"cannot read the ledger {path}: {error}") from error

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = {"attempts": [asdict(attempt) for attempt in self.attempts]}
        path.write_text(json.dumps(payload, indent=2) + "\n")

    def record(self, attempt: Attempt) -> None:
        self.attempts.append(attempt)

    def outcome(self, cluster: str) -> str | None:
        matches = [a.outcome for a in self.attempts if a.cluster == cluster]
        return matches[-1] if matches else None

    def consecutive_rejections(self) -> int:
        count = 0
        for attempt in reversed(self.attempts):
            if attempt.outcome != "rejected":
                break
            count += 1
        return count


@dataclass(frozen=True)
class TrendRow:
    date: str
    pr: int
    kind: str
    nloc_before: int
    nloc_after: int


def append_trend(path: Path, row: TrendRow) -> None:
    if row.nloc_after >= row.nloc_before:
        raise NlocLoopError(f"a loop PR must lower NLOC: {row.nloc_before} -> {row.nloc_after}")
    new = not path.exists()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        if new:
            writer.writerow(TREND_HEADER)
        writer.writerow(asdict(row).values())


@dataclass(frozen=True)
class Occurrence:
    path: str
    start: int
    end: int

    @property
    def lines(self) -> int:
        return self.end - self.start + 1


@dataclass(frozen=True)
class Candidate:
    cluster_id: str
    kind: str
    occurrences: tuple[Occurrence, ...]

    @property
    def savings(self) -> int:
        """Lines removed by keeping one copy; the shortest occurrence bounds each removal."""
        return (len(self.occurrences) - 1) * min(o.lines for o in self.occurrences)


def _relative(root: Path, name: str) -> str:
    """Repository-relative path; jscpd may report the root with symlinks resolved or not."""
    for base in (root, root.resolve()):
        if Path(name).is_relative_to(base):
            return Path(name).relative_to(base).as_posix()
    raise NlocLoopError(f"jscpd reported {name}, outside {root}")


def _coalesce(fragments: Sequence[Occurrence]) -> list[Occurrence]:
    """Merge overlapping fragments of one file into single occurrences."""
    merged: list[Occurrence] = []
    for fragment in sorted(fragments, key=lambda o: (o.path, o.start)):
        last = merged[-1] if merged else None
        if last is not None and last.path == fragment.path and fragment.start <= last.end:
            merged[-1] = Occurrence(last.path, last.start, max(last.end, fragment.end))
        else:
            merged.append(fragment)
    return merged


def _cluster_id(occurrences: Sequence[Occurrence], texts: Sequence[str]) -> str:
    """Stable across line shifts: the files involved and the shortest duplicated text."""
    text = min((" ".join(t.split()) for t in texts), key=lambda t: (len(t), t))
    key = "\n".join([*sorted(o.path for o in occurrences), text])
    return hashlib.sha1(key.encode()).hexdigest()[:12]


def find_candidates(
    report: Mapping, root: Path, host_files: set[str], ledger: Ledger
) -> list[Candidate]:
    """Cluster jscpd pairs, drop ineligible clusters, and rank the queue."""
    fragments: list[Occurrence] = []
    texts: list[str] = []
    parent: list[int] = []

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for duplicate in report["duplicates"]:
        for side in ("firstFile", "secondFile"):
            f = duplicate[side]
            fragments.append(Occurrence(_relative(root, f["name"]), f["start"], f["end"]))
            texts.append(duplicate.get("fragment", ""))
            parent.append(len(parent))
        parent[find(len(parent) - 1)] = find(len(parent) - 2)
    for i, a in enumerate(fragments):
        for j in range(i):
            b = fragments[j]
            if a.path == b.path and a.start <= b.end and b.start <= a.end:
                parent[find(i)] = find(j)

    components: dict[int, list[int]] = {}
    for i in range(len(fragments)):
        components.setdefault(find(i), []).append(i)

    candidates = []
    for members in components.values():
        occurrences = [
            o for o in _coalesce([fragments[i] for i in members]) if o.path in host_files
        ]
        if len(occurrences) < 2:
            continue
        kinds = {classify(o.path) for o in occurrences}
        candidate = Candidate(
            _cluster_id(occurrences, [texts[i] for i in members]),
            "test-only" if kinds == {TEST} else "production",
            tuple(occurrences),
        )
        if candidate.savings >= MIN_SAVINGS and ledger.outcome(candidate.cluster_id) is None:
            candidates.append(candidate)
    return sorted(candidates, key=lambda c: (c.kind != "test-only", -c.savings, c.cluster_id))


def run_jscpd_hook(root: Path) -> Mapping:
    report = root / JSCPD_REPORT
    report.unlink(missing_ok=True)
    _run_tool(root, ["prek", "run", "jscpd", "--hook-stage", "manual", "--all-files"])
    try:
        return json.loads(report.read_text())
    except (OSError, ValueError) as error:
        raise NlocLoopError(f"cannot read the jscpd report {report}: {error}") from error


def parse_host_files(output: str) -> set[str]:
    files = set()
    for line in output.splitlines():
        if line.startswith("//"):
            package, name = line.split(" ", 1)[0][2:].split(":", 1)
            files.add(f"{package}/{name}" if package else name)
    return files


def query_host_files(root: Path) -> set[str]:
    """First-party sources reachable from targets compatible with the host platform."""
    query = 'kind("source file", deps(//src/... + //apps/... + //tests/...))'
    return parse_host_files(_run_tool(root, ["bazel", "cquery", query, "--output=label"]))


def _run_tool(root: Path, command: Sequence[str]) -> str:
    tool = shutil.which(command[0])
    if tool is None:
        raise NlocLoopError(f"{command[0]} was not found on PATH")
    result = subprocess.run(
        [tool, *command[1:]], cwd=root, capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise NlocLoopError(f"{' '.join(command)} failed:\n{result.stdout}{result.stderr}")
    return result.stdout


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="repository root")
    commands = parser.add_subparsers(dest="command", required=True)

    measure_parser = commands.add_parser("measure", help="print NLOC and its split")
    measure_parser.add_argument("--json", action="store_true")

    ledger_parser = commands.add_parser("ledger", help="record or list attempted clusters")
    ledger_commands = ledger_parser.add_subparsers(dest="ledger_command", required=True)
    record = ledger_commands.add_parser("record")
    record.add_argument("cluster")
    record.add_argument("outcome", choices=OUTCOMES)
    record.add_argument("--reason", default="")
    record.add_argument("--pr", type=int)
    ledger_commands.add_parser("list")

    commands.add_parser("candidates", help="print the ranked duplicate-cluster queue")

    trend = commands.add_parser("trend", help="append the current NLOC to the trend log")
    trend.add_argument("--pr", type=int, required=True)
    trend.add_argument("--kind", choices=KINDS, required=True)
    trend.add_argument("--before", type=int, required=True, help="NLOC of the base")
    return parser.parse_args(argv)


def _run(
    args: argparse.Namespace,
    run_hook: RunHook,
    run_jscpd: RunJscpd,
    host_files: HostFiles,
    out: TextIO,
    today: str,
) -> None:
    root = args.root
    if args.command == "candidates":
        ledger = Ledger.load(root / LEDGER)
        for c in find_candidates(run_jscpd(root), root, host_files(root), ledger):
            out.write(
                f"{c.cluster_id} {c.kind} savings={c.savings} occurrences={len(c.occurrences)}\n"
            )
            out.writelines(f"  {o.path}:{o.start}-{o.end}\n" for o in c.occurrences)
    elif args.command == "measure":
        result = measure(root, run_hook)
        if args.json:
            out.write(json.dumps({"total": result.total, **asdict(result)}) + "\n")
        else:
            out.write(f"total {result.total}\nproduction {result.production}\ntest {result.test}\n")
    elif args.command == "ledger":
        ledger = Ledger.load(root / LEDGER)
        if args.ledger_command == "record":
            ledger.record(Attempt(args.cluster, args.outcome, today, args.reason, args.pr))
            ledger.save(root / LEDGER)
        else:
            for a in ledger.attempts:
                out.write(
                    " ".join(
                        str(v) for v in (a.date, a.cluster, a.outcome, a.reason, a.pr or "") if v
                    )
                    + "\n"
                )
            out.write(f"consecutive rejections: {ledger.consecutive_rejections()}\n")
    else:
        after = measure(root, run_hook).total
        append_trend(root / TREND, TrendRow(today, args.pr, args.kind, args.before, after))


def main(
    argv: Sequence[str] | None = None,
    run_hook: RunHook = run_nloc_hook,
    run_jscpd: RunJscpd = run_jscpd_hook,
    host_files: HostFiles = query_host_files,
    out: TextIO = sys.stdout,
    today: Callable[[], str] = lambda: datetime.date.today().isoformat(),
) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        _run(args, run_hook, run_jscpd, host_files, out, today())
    except NlocLoopError as error:
        out.write(f"nloc_loop: {error}\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
