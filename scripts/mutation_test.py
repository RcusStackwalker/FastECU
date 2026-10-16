#!/usr/bin/env python3
"""Run Mull mutation testing over the portable tests and report merged results.

Builds the selected portable test binaries once with Mull's compiler plugin
(`--config=mutation`), runs `mull-runner` on each, and merges the per-binary
Mutation Testing Elements reports: a mutant counts as killed when any binary
kills it. Writes a Markdown summary and, with --annotate, GitHub `::warning`
annotations for surviving mutants.

Modes:
  full  (default)  every portable cc_test, every mutant under src/algorithms
                   and src/backend.
  --diff-ref REF   only tests that depend on portable files changed since
                   REF, and only mutants on the changed lines.

Surviving mutants never fail the run (mutation testing is advisory); a tool or
build failure does. Exit codes: 0 = ran (or nothing to do), 2 = tool failure.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path

PORTABLE_PREFIXES = ("src/algorithms/", "src/backend/")
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")
PORTABLE_SCOPE = "//src/algorithms/... + //src/backend/..."
# GitHub shows at most 10 warning annotations per step.
MAX_ANNOTATIONS = 10
# A mutant's merged status is the strongest any binary reports for it.
_STATUS_RANK = {"NoCoverage": 0, "Survived": 1, "Timeout": 2, "Killed": 3}
_RUNNER = re.compile(r"^mull-runner-(\d+)$")


class MutationTestError(RuntimeError):
    """A tool, build, or report failure: not a surviving mutant."""


@dataclasses.dataclass(frozen=True, order=True)
class MutantKey:
    path: str
    line: int
    column: int
    mutator: str
    replacement: str


@dataclasses.dataclass
class FileCounts:
    killed: int = 0
    survived: int = 0
    no_coverage: int = 0

    @property
    def score(self) -> float | None:
        covered = self.killed + self.survived
        return self.killed / covered if covered else None


def relative_source_path(path: str, root: Path) -> str:
    """Map a path recorded in debug info to a workspace-relative one."""
    root_prefix = str(root).rstrip("/") + "/"
    if path.startswith(root_prefix):
        return path[len(root_prefix) :]
    marker = "/execroot/_main/"
    if marker in path:
        return path.split(marker, 1)[1]
    return path.removeprefix("./")


def merge_reports(reports: Iterable[Mapping], root: Path) -> dict[MutantKey, str]:
    """Merge Elements reports; the strongest status per mutant wins."""
    merged: dict[MutantKey, str] = {}
    for report in reports:
        for path, entry in report.get("files", {}).items():
            relative = relative_source_path(path, root)
            for mutant in entry.get("mutants", []):
                start = mutant["location"]["start"]
                key = MutantKey(
                    relative,
                    start["line"],
                    start["column"],
                    mutant["mutatorName"],
                    mutant.get("replacement", ""),
                )
                status = mutant["status"]
                if status not in _STATUS_RANK:
                    raise MutationTestError(f"unknown mutant status {status!r} in {path}")
                previous = merged.get(key)
                if previous is None or _STATUS_RANK[status] > _STATUS_RANK[previous]:
                    merged[key] = status
    return merged


def count_by_file(merged: Mapping[MutantKey, str]) -> dict[str, FileCounts]:
    counts: dict[str, FileCounts] = {}
    for key, status in merged.items():
        file_counts = counts.setdefault(key.path, FileCounts())
        if status in ("Killed", "Timeout"):
            file_counts.killed += 1
        elif status == "Survived":
            file_counts.survived += 1
        else:
            file_counts.no_coverage += 1
    return counts


def _percent(score: float | None) -> str:
    return "n/a" if score is None else f"{score:.0%}"


def render_summary(merged: Mapping[MutantKey, str], title: str) -> str:
    counts = count_by_file(merged)
    total = FileCounts()
    for file_counts in counts.values():
        total.killed += file_counts.killed
        total.survived += file_counts.survived
        total.no_coverage += file_counts.no_coverage
    lines = [f"## {title}", ""]
    if not merged:
        lines.append("No mutants in scope.")
        return "\n".join(lines) + "\n"
    lines += [
        f"Mutation score **{_percent(total.score)}**: {total.killed} killed, "
        f"{total.survived} survived, {total.no_coverage} not covered by any test.",
        "",
        "| File | Killed | Survived | Not covered | Score |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    ranked = sorted(counts.items(), key=lambda item: (-item[1].survived, item[0]))
    for path, file_counts in ranked:
        lines.append(
            f"| `{path}` | {file_counts.killed} | {file_counts.survived} | "
            f"{file_counts.no_coverage} | {_percent(file_counts.score)} |"
        )
    return "\n".join(lines) + "\n"


def render_annotations(merged: Mapping[MutantKey, str], limit: int = MAX_ANNOTATIONS) -> list[str]:
    """GitHub warning annotations for surviving mutants, in source order."""
    survivors = sorted(key for key, status in merged.items() if status == "Survived")
    annotations = [
        f"::warning file={key.path},line={key.line},col={key.column},"
        f"title=Surviving mutant::[{key.mutator}] replaced with `{key.replacement}`; "
        "no test failed"
        for key in survivors[:limit]
    ]
    if len(survivors) > limit:
        annotations.append(
            f"::notice::{len(survivors) - limit} more surviving mutants; see the job summary"
        )
    return annotations


def changed_portable_sources(changed: Iterable[str]) -> list[str]:
    """Changed first-party portable sources and headers, excluding tests."""
    return sorted(
        path
        for path in changed
        if path.startswith(PORTABLE_PREFIXES)
        and path.endswith(SOURCE_SUFFIXES)
        and not Path(path).stem.endswith("_test")
        and "/testing/" not in path
    )


def diff_config(base_config: str, diff_ref: str, root: Path) -> str:
    """The checked-in Mull config plus incremental (git diff) filtering."""
    return (
        base_config.rstrip("\n")
        + f"\ngitDiffRef: {json.dumps(diff_ref)}\ngitProjectRoot: {json.dumps(str(root))}\n"
    )


def binary_path(root: Path, label: str) -> Path:
    package, _, name = label.removeprefix("//").partition(":")
    return root / "bazel-bin" / package / name


# ---- Environment: tools, Bazel, Mull ------------------------------------------------


def _run(command: Sequence[str], root: Path, **kwargs) -> subprocess.CompletedProcess:
    print("+", " ".join(command), flush=True)
    return subprocess.run(command, cwd=root, check=False, text=True, **kwargs)


def find_mull(environ: Mapping[str, str]) -> tuple[str, str]:
    """Locate mull-runner-N and its matching mull-ir-frontend-N plugin."""
    runner = environ.get("MULL_RUNNER")
    if runner is None:
        candidates = sorted(
            (int(match.group(1)), entry.name)
            for directory in environ.get("PATH", "").split(os.pathsep)
            if directory and Path(directory).is_dir()
            for entry in Path(directory).iterdir()
            if (match := _RUNNER.match(entry.name))
        )
        if not candidates:
            raise MutationTestError("no mull-runner-<N> on PATH; set MULL_RUNNER")
        runner = shutil.which(candidates[-1][1], path=environ.get("PATH"))
    match = _RUNNER.match(Path(runner).name)
    plugin = environ.get("MULL_IR_FRONTEND")
    if plugin is None and match:
        runner_prefix = Path(runner).resolve().parent.parent
        name = f"mull-ir-frontend-{match.group(1)}"
        for candidate in (Path("/usr/lib") / name, runner_prefix / "lib" / name):
            if candidate.exists():
                plugin = str(candidate)
                break
    if plugin is None:
        raise MutationTestError(
            f"cannot find the IR frontend plugin for {runner}; set MULL_IR_FRONTEND"
        )
    return str(runner), plugin


def tests_expression(scope: str, changed: Sequence[str] | None) -> str:
    """Non-fuzz cc_tests in scope; with `changed`, only those depending on it."""
    expression = f'kind(cc_test, {scope}) except attr(tags, "\\bfuzz\\b", {scope})'
    if changed is not None:
        # Changed files name themselves as file targets; a header no target
        # lists is skipped under --keep_going (exit code 3, partial result).
        expression = f"({expression}) intersect rdeps({scope}, set({' '.join(changed)}))"
    return expression


def query_tests(root: Path, scope: str, changed: Sequence[str] | None) -> list[str]:
    expression = tests_expression(scope, changed)
    result = _run(
        ["bazel", "query", "--keep_going", expression, "--output=label"], root, capture_output=True
    )
    if result.returncode not in (0, 3):
        raise MutationTestError(f"bazel query failed:\n{result.stderr}")
    return [line for line in result.stdout.splitlines() if line.startswith("//")]


def git_changed_files(root: Path, diff_ref: str) -> list[str]:
    result = _run(
        ["git", "diff", "--name-only", "--diff-filter=d", f"{diff_ref}...HEAD"],
        root,
        capture_output=True,
    )
    if result.returncode:
        raise MutationTestError(f"git diff against {diff_ref} failed:\n{result.stderr}")
    return result.stdout.splitlines()


def build_tests(root: Path, targets: Sequence[str], plugin: str, config_path: Path) -> None:
    result = _run(
        [
            "bazel",
            "build",
            "--config=mutation",
            f"--copt=-fpass-plugin={plugin}",
            f"--action_env=MULL_CONFIG={config_path}",
            *targets,
        ],
        root,
    )
    if result.returncode:
        raise MutationTestError("building the mutated tests failed")


def run_mull(
    root: Path, runner: str, label: str, config_path: Path, report_dir: Path
) -> Mapping | None:
    binary = binary_path(root, label)
    runfiles = binary.parent / f"{binary.name}.runfiles" / "_main"
    environ = {**os.environ, "MULL_CONFIG": str(config_path)}
    result = _run(
        [
            runner,
            "--reporters",
            "Elements",
            "--report-dir",
            str(report_dir),
            "--report-name",
            binary.name,
            "--no-test-output",
            "--no-mutant-output",
            str(binary),
        ],
        runfiles if runfiles.is_dir() else root,
        env=environ,
    )
    if result.returncode:
        raise MutationTestError(f"mull-runner failed on {label}")
    report = report_dir / f"{binary.name}.json"
    # The runner writes no report when the git diff filter left nothing to run.
    return json.loads(report.read_text()) if report.exists() else None


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--diff-ref", help="mutate only lines changed since this git ref")
    parser.add_argument("--report-dir", type=Path, help="keep per-binary reports here")
    parser.add_argument("--summary", type=Path, help="append the Markdown summary to this file")
    parser.add_argument("--annotate", action="store_true", help="print GitHub annotations")
    parser.add_argument(
        "--scope",
        default=PORTABLE_SCOPE,
        help=f"Bazel target pattern whose tests run (default: {PORTABLE_SCOPE})",
    )
    return parser.parse_args(argv)


def _emit(summary: str, merged: Mapping[MutantKey, str], args: argparse.Namespace) -> None:
    print(summary)
    if args.summary:
        with args.summary.open("a") as handle:
            handle.write(summary)
    if args.annotate:
        for line in render_annotations(merged):
            print(line)


def main(argv: Sequence[str] = sys.argv[1:]) -> int:
    args = _parse_args(argv)
    root = Path(__file__).resolve().parent.parent
    base_config = (root / "mull.yml").read_text()
    title = (
        f"Mutation testing (lines changed since {args.diff_ref})"
        if args.diff_ref
        else "Mutation testing"
    )
    try:
        changed = None
        if args.diff_ref:
            changed = changed_portable_sources(git_changed_files(root, args.diff_ref))
            if not changed:
                _emit(f"## {title}\n\nNo portable sources changed.\n", {}, args)
                return 0
        runner, plugin = find_mull(os.environ)
        targets = query_tests(root, args.scope, changed)
        if not targets:
            _emit(f"## {title}\n\nNo portable tests depend on the change.\n", {}, args)
            return 0
        with tempfile.TemporaryDirectory(prefix="fastecu-mull-") as scratch:
            report_dir = args.report_dir or Path(scratch) / "reports"
            report_dir.mkdir(parents=True, exist_ok=True)
            config_path = root / "mull.yml"
            if args.diff_ref:
                config_path = Path(scratch) / "mull.yml"
                config_path.write_text(diff_config(base_config, args.diff_ref, root))
            # The build always uses the checked-in config, so every mode shares one
            # set of mutated binaries; the runner applies the diff filter.
            build_tests(root, targets, plugin, root / "mull.yml")
            reports = [
                report
                for label in targets
                if (report := run_mull(root, runner, label, config_path, report_dir))
            ]
        merged = merge_reports(reports, root)
        _emit(render_summary(merged, title), merged, args)
        return 0
    except MutationTestError as error:
        print(f"mutation_test: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
