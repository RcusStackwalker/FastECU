#!/usr/bin/env python3
"""Measure NLOC, rank duplicate clusters, check guards, and keep the hill-climbing loop's records.

See docs/tooling.md#nloc-hill-climbing-loop. NLOC comes from the pinned `nloc`
prek hook (Lizard) and duplicates from the pinned `jscpd` hook. Exit codes:
0 = success, 1 = a guard failed, 2 = tool or input failure.
"""

from __future__ import annotations

import argparse
import contextlib
import csv
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Mapping, Sequence
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import TextIO
from xml.etree import ElementTree

import mutation_test

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
SNAPSHOTS = Path(".nloc-loop/snapshots")
RunHook = Callable[[Path], str]
RunJscpd = Callable[[Path], Mapping]
HostFiles = Callable[[Path], set[str]]
RunCoverage = Callable[[Path], tuple[str, list[str]]]
RunGit = Callable[[Path, Sequence[str]], str]
QueryTests = Callable[[Path, str, Sequence[str]], list[str]]
# Mull statuses that count as a kill, as in mutation_test.count_by_file.
KILLED = ("Killed", "Timeout")

_FILE_HEADER = re.compile(r"^\s*NLOC\s+Avg\.NLOC\s+AvgCCN\s+Avg\.token\s+function_cnt\s+file\s*$")
_FILE_ROW = re.compile(r"^\s*(\d+)\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+\d+\s+(?:\./)?(\S.*?)\s*$")
_TOTAL_HEADER = re.compile(r"^\s*Total nloc\s")
_TOTAL_ROW = re.compile(r"^\s*(\d+)\s")
_TEST_NAME = re.compile(r"_test[._]")
_COVERAGE_ROW = re.compile(r"^\s*(\d+)\|\s*([0-9.]+[kMGTPE]?)?\s*\|")


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
    """Repository-relative path; tools may report the root with symlinks resolved or not."""
    for base in (root, root.resolve()):
        if Path(name).is_relative_to(base):
            return Path(name).relative_to(base).as_posix()
    raise NlocLoopError(f"a tool reported {name}, outside {root}")


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


@dataclass(frozen=True)
class FileCoverage:
    covered: frozenset[int]
    executable: int


def parse_coverage(report: str, root: Path) -> dict[str, FileCoverage]:
    """Per-file covered lines from `llvm-cov show`; instantiation sub-views are skipped."""
    sections: dict[str, tuple[set[int], list[int]]] = {}
    current = None
    for line in report.splitlines():
        if line.endswith(":") and not line[:1].isspace():
            current = sections.setdefault(_relative(root, line[:-1]), (set(), [0]))
            continue
        row = _COVERAGE_ROW.match(line)
        if current is None or row is None or row.group(2) is None:
            continue
        current[1][0] += 1
        if row.group(2) != "0":
            current[0].add(int(row.group(1)))
    return {
        path: FileCoverage(frozenset(covered), executable[0])
        for path, (covered, executable) in sections.items()
    }


def count_test_cases(xmls: Sequence[str]) -> int:
    """Executed test cases across Bazel test.xml files; skipped and disabled cases excluded."""
    count = 0
    for text in xmls:
        try:
            cases = ElementTree.fromstring(text).iter("testcase")
        except ElementTree.ParseError as error:
            raise NlocLoopError(f"malformed test.xml: {error}") from error
        count += sum(
            case.get("status", "run") == "run"
            and case.get("result") not in ("skipped", "suppressed")
            and case.find("skipped") is None
            for case in cases
        )
    return count


@dataclass(frozen=True)
class Snapshot:
    commit: str
    test_cases: int
    coverage: Mapping[str, FileCoverage]

    @classmethod
    def load(cls, path: Path) -> Snapshot:
        data = json.loads(path.read_text())
        return cls(
            data["commit"],
            data["test_cases"],
            {
                p: FileCoverage(frozenset(c["covered"]), c["executable"])
                for p, c in data["coverage"].items()
            },
        )

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        coverage = {
            p: {"covered": sorted(c.covered), "executable": c.executable}
            for p, c in sorted(self.coverage.items())
        }
        payload = {"commit": self.commit, "test_cases": self.test_cases, "coverage": coverage}
        path.write_text(json.dumps(payload) + "\n")


@dataclass(frozen=True)
class GuardResult:
    name: str
    ok: bool
    detail: str


def _lost_lines(base: Snapshot, head: Snapshot) -> dict[str, list[int]]:
    lost = {}
    for path, coverage in base.coverage.items():
        if classify(path) != PRODUCTION:
            continue
        kept = head.coverage.get(path, FileCoverage(frozenset(), 0)).covered
        if missing := sorted(coverage.covered - kept):
            lost[path] = missing
    return lost


def _worse_files(base: Snapshot, head: Snapshot, changed: Sequence[str]) -> list[str]:
    worse = []
    for path in changed:
        before, after = base.coverage.get(path), head.coverage.get(path)
        if classify(path) != PRODUCTION or before is None or after is None:
            continue
        # Cross-multiplied so equal fractions compare equal.
        if len(after.covered) * before.executable < len(before.covered) * after.executable:
            worse.append(
                f"{path}: {len(before.covered)}/{before.executable}"
                f" -> {len(after.covered)}/{after.executable}"
            )
    return worse


def check_guards(
    base: Snapshot, head: Snapshot, kind: str, changed: Sequence[str]
) -> list[GuardResult]:
    """The local guards for a loop PR of `kind` (docs/tooling.md#nloc-hill-climbing-loop)."""
    results = [
        GuardResult(
            "test cases",
            head.test_cases >= base.test_cases,
            f"{base.test_cases} -> {head.test_cases}",
        )
    ]
    if kind == "test-only":
        touched = [p for p in changed if classify(p) == PRODUCTION]
        results.append(GuardResult("production unchanged", not touched, " ".join(touched)))
        lost = _lost_lines(base, head)
        detail = "; ".join(
            f"{p}: {', '.join(map(str, lines[:10]))}{' ...' if len(lines) > 10 else ''}"
            for p, lines in sorted(lost.items())
        )
        results.append(GuardResult("covered lines", not lost, detail))
    else:
        worse = _worse_files(base, head, changed)
        results.append(GuardResult("touched coverage", not worse, "; ".join(worse)))
    return results


def run_coverage_local(root: Path) -> tuple[str, list[str]]:
    """Run scripts/coverage-local.sh into .nloc-loop/coverage; return its report and test.xmls."""
    started = time.time()
    output = root / SNAPSHOTS.parent / "coverage"
    result = subprocess.run(
        [str(root / "scripts" / "coverage-local.sh")],
        cwd=root,
        env={**os.environ, "COVERAGE_DIR": str(output)},
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        tail = "\n".join((result.stdout + result.stderr).splitlines()[-40:])
        raise NlocLoopError(f"coverage-local.sh failed:\n{tail}")
    xmls = [
        p.read_text()
        for p in (root / "bazel-testlogs").resolve().rglob("test.xml")
        if p.stat().st_mtime >= started
    ]
    return (output / "llvm-cov.report").read_text(), xmls


def run_git(root: Path, args: Sequence[str]) -> str:
    return _run_tool(root, ["git", *args])


def _snapshot_path(root: Path, commit: str) -> Path:
    return root / SNAPSHOTS / f"{commit}.json"


def _load_snapshot(root: Path, commit: str, role: str) -> Snapshot:
    path = _snapshot_path(root, commit)
    if not path.exists():
        raise NlocLoopError(
            f"no snapshot for {role} {commit}; check it out and run `guards snapshot`"
        )
    return Snapshot.load(path)


def _guards(args: argparse.Namespace, run_coverage: RunCoverage, git: RunGit, out: TextIO) -> int:
    root = args.root
    if args.guards_command == "mutation":
        return _mutation_guard(args, out)
    head = git(root, ["rev-parse", "HEAD"]).strip()
    if args.guards_command == "snapshot":
        if git(root, ["status", "--porcelain", "--untracked-files=no"]).strip():
            raise NlocLoopError(
                "a snapshot describes a commit; commit or stash uncommitted changes"
            )
        report, xmls = run_coverage(root)
        snap = Snapshot(head, count_test_cases(xmls), parse_coverage(report, root))
        snap.save(_snapshot_path(root, head))
        out.write(f"snapshot {head}: {snap.test_cases} test cases, {len(snap.coverage)} files\n")
        return 0
    base = git(root, ["merge-base", args.base, "HEAD"]).strip()
    changed = git(root, ["diff", "--name-only", base, head]).split()
    results = check_guards(
        _load_snapshot(root, base, "base"), _load_snapshot(root, head, "head"), args.kind, changed
    )
    for r in results:
        out.write(f"{'PASS' if r.ok else 'FAIL'} {r.name}{': ' + r.detail if r.detail else ''}\n")
    return 0 if all(r.ok for r in results) else 1


def pr_kind(changed: Sequence[str]) -> str:
    """A loop PR is a production PR as soon as it changes one production file."""
    return "production" if any(classify(p) == PRODUCTION for p in changed) else "test-only"


def portable_test_sources(changed: Sequence[str], root: Path) -> list[str]:
    """Changed portable test sources present in the checkout: Mull's test selection."""
    return sorted(
        path
        for path in changed
        if path.startswith(mutation_test.PORTABLE_PREFIXES)
        and path.endswith(mutation_test.SOURCE_SUFFIXES)
        and classify(path) == TEST
        and (root / path).exists()
    )


MutantKey = mutation_test.MutantKey


def lost_kills(base: Mapping[MutantKey, str], head: Mapping[MutantKey, str]) -> list[MutantKey]:
    """Mutants the base's tests kill that the head's tests no longer kill."""
    return sorted(k for k, s in base.items() if s in KILLED and head.get(k) not in KILLED)


def survivors(merged: Mapping[MutantKey, str]) -> list[MutantKey]:
    return sorted(k for k, s in merged.items() if s == "Survived")


def _describe_mutants(keys: Sequence[MutantKey]) -> str:
    shown = [f"{k.path}:{k.line}:{k.column} {k.mutator}" for k in keys[:10]]
    return "; ".join(shown) + (f"; ... {len(keys) - 10} more" if len(keys) > 10 else "")


def _merged_shards(directory: Path, shards: int, root: Path) -> dict[MutantKey, str]:
    reports, missing = mutation_test.collect_shard_reports(directory, shards)
    if missing:
        raise NlocLoopError(f"incomplete mutation run in {directory}: missing shards {missing}")
    return mutation_test.merge_reports(reports, root)


def _mutation_guard(args: argparse.Namespace, out: TextIO) -> int:
    root = args.root
    if args.base_reports is None:
        merged = mutation_test.merge_reports(mutation_test.read_reports(args.reports), root)
        survived = survivors(merged)
        result = GuardResult("surviving mutants", not survived, _describe_mutants(survived) or "0")
    else:
        base = _merged_shards(args.base_reports, args.shards, root)
        head = _merged_shards(args.reports, args.shards, root)
        lost = lost_kills(base, head)
        kills = [sum(s in KILLED for s in m.values()) for m in (base, head)]
        detail = _describe_mutants(lost) if lost else f"{kills[0]} -> {kills[1]}"
        result = GuardResult("killed mutants", not lost, detail)
    out.write(f"{'PASS' if result.ok else 'FAIL'} {result.name}: {result.detail}\n")
    return 0 if result.ok else 1


def query_portable_tests(root: Path, scope: str, changed: Sequence[str]) -> list[str]:
    """mutation_test's test query, with its command echo kept off stdout."""
    with contextlib.redirect_stdout(sys.stderr):
        return mutation_test.query_tests(root, scope, changed)


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

    guards = commands.add_parser("guards", help="snapshot coverage or check a loop PR")
    guards_commands = guards.add_subparsers(dest="guards_command", required=True)
    guards_commands.add_parser("snapshot", help="record HEAD's test cases and coverage")
    check = guards_commands.add_parser("check", help="compare HEAD with its base")
    check.add_argument("--kind", choices=KINDS, required=True)
    check.add_argument("--base", default="origin/master", help="ref whose merge-base is the base")
    mutation = guards_commands.add_parser(
        "mutation", help="check Mull reports: kills kept (with a base) or no survivors"
    )
    mutation.add_argument("--reports", type=Path, required=True, help="the PR's report dir")
    mutation.add_argument("--base-reports", type=Path, help="the base's sharded report dir")
    mutation.add_argument("--shards", type=int, default=1, help="shards per sharded run")

    for name, help_text in (
        ("pr-kind", "print whether base..head is a test-only or production change"),
        ("mutation-scope", "print the portable tests depending on changed test sources"),
    ):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("--base", type=mutation_test.operand, required=True)
        command.add_argument("--head", type=mutation_test.operand, required=True)

    trend = commands.add_parser("trend", help="append the current NLOC to the trend log")
    trend.add_argument("--pr", type=int, required=True)
    trend.add_argument("--kind", choices=KINDS, required=True)
    trend.add_argument("--before", type=int, required=True, help="NLOC of the base")
    return parser.parse_args(argv)


@dataclass(frozen=True)
class Tools:
    """The external tools each command runs; tests substitute them."""

    run_hook: RunHook
    run_jscpd: RunJscpd
    host_files: HostFiles
    run_coverage: RunCoverage
    git: RunGit
    query_tests: QueryTests


def _run(args: argparse.Namespace, tools: Tools, out: TextIO, today: str) -> int:
    root = args.root
    if args.command in ("pr-kind", "mutation-scope"):
        changed = tools.git(root, ["diff", "--name-only", args.base, args.head]).split()
        if args.command == "pr-kind":
            out.write(pr_kind(changed) + "\n")
        elif sources := portable_test_sources(changed, root):
            labels = tools.query_tests(root, mutation_test.PORTABLE_SCOPE, sources)
            out.writelines(f"{label}\n" for label in labels)
        return 0
    if args.command == "guards":
        return _guards(args, tools.run_coverage, tools.git, out)
    if args.command == "candidates":
        ledger = Ledger.load(root / LEDGER)
        for c in find_candidates(tools.run_jscpd(root), root, tools.host_files(root), ledger):
            out.write(
                f"{c.cluster_id} {c.kind} savings={c.savings} occurrences={len(c.occurrences)}\n"
            )
            out.writelines(f"  {o.path}:{o.start}-{o.end}\n" for o in c.occurrences)
    elif args.command == "measure":
        result = measure(root, tools.run_hook)
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
        after = measure(root, tools.run_hook).total
        append_trend(root / TREND, TrendRow(today, args.pr, args.kind, args.before, after))
    return 0


def main(
    argv: Sequence[str] | None = None,
    run_hook: RunHook = run_nloc_hook,
    run_jscpd: RunJscpd = run_jscpd_hook,
    host_files: HostFiles = query_host_files,
    run_coverage: RunCoverage = run_coverage_local,
    run_git: RunGit = run_git,
    query_tests: QueryTests = query_portable_tests,
    out: TextIO = sys.stdout,
    today: Callable[[], str] = lambda: datetime.date.today().isoformat(),
) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        tools = Tools(run_hook, run_jscpd, host_files, run_coverage, run_git, query_tests)
        return _run(args, tools, out, today())
    except (NlocLoopError, mutation_test.MutationTestError) as error:
        out.write(f"nloc_loop: {error}\n")
        return 2


if __name__ == "__main__":
    sys.exit(main())
