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
  --shard I/N      only every Nth test of the sorted selection, from the Ith;
                   writes reports and a manifest for --merge-reports.
  --merge-reports  merge the reports of N shards' report directories.

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
CONFIG_NAME = "mull.yml"
# GitHub shows at most 10 warning annotations per step.
MAX_ANNOTATIONS = 10
# A mutant's merged status is the strongest any binary reports for it.
_STATUS_RANK = {"NoCoverage": 0, "Survived": 1, "Timeout": 2, "Killed": 3}
_RUNNER = re.compile(r"^mull-runner-(\d+)$")
# Limit for mull-runner's unmutated warm-up and baseline runs. Its 3000 ms
# default kills checksum_test, whose full-size ROMs take seconds in the -O0
# coverage build. Mutant runs are unaffected: they get 10x the baseline time.
ORIGINAL_RUN_TIMEOUT_MS = 60_000
# Each test's `env` attribute as `bazel test` would set it. mull-runner runs
# the binary directly, so the script sets it instead: a test reading a data
# path from its environment fails its unmutated run without it.
TEST_ENV_EXPR = (
    'json.encode({"label": str(target.label), "env": '
    'providers(target)["RunEnvironmentInfo"].environment '
    'if "RunEnvironmentInfo" in providers(target) else {}})'
)


# Written last by a shard, so a shard that failed or was cancelled has none.
SHARD_MANIFEST = "_shard.json"


class MutationTestError(RuntimeError):
    """A tool, build, or report failure: not a surviving mutant."""


@dataclasses.dataclass(frozen=True)
class Shard:
    index: int
    count: int

    def select(self, labels: Iterable[str]) -> list[str]:
        """Round-robin over the sorted labels, so every shard agrees on its slice."""
        return sorted(labels)[self.index :: self.count]


def parse_shard(value: str) -> Shard:
    index, _, count = value.partition("/")
    if not (index.isdigit() and count.isdigit()) or not int(index) < int(count):
        raise argparse.ArgumentTypeError(f"{value!r} is not I/N with 0 <= I < N")
    return Shard(int(index), int(count))


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
    # Debug-info and git paths are POSIX; so is this comparison on any host.
    root_prefix = root.as_posix().rstrip("/") + "/"
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


def _describe(replacement: str) -> str:
    return f"replaced with `{replacement}`" if replacement else "removed"


def render_annotations(merged: Mapping[MutantKey, str], limit: int = MAX_ANNOTATIONS) -> list[str]:
    """GitHub warning annotations for surviving mutants, in source order."""
    survivors = sorted(key for key, status in merged.items() if status == "Survived")
    annotations = [
        f"::warning file={key.path},line={key.line},col={key.column},"
        f"title=Surviving mutant::[{key.mutator}] {_describe(key.replacement)}; no test failed"
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
        + f"\ngitDiffRef: {json.dumps(diff_ref)}\ngitProjectRoot: {json.dumps(root.as_posix())}\n"
    )


def write_shard_manifest(report_dir: Path, shard: Shard, labels: Sequence[str]) -> None:
    manifest = {"index": shard.index, "count": shard.count, "labels": list(labels)}
    (report_dir / SHARD_MANIFEST).write_text(json.dumps(manifest))


def collect_shard_reports(directory: Path, count: int) -> tuple[list[Mapping], list[int]]:
    """Every report under `directory`, and the shards that left no manifest."""
    reports, finished = [], set()
    for path in sorted(directory.rglob("*.json")):
        data = json.loads(path.read_text())
        if path.name != SHARD_MANIFEST:
            reports.append(data)
        elif data["count"] != count:
            raise MutationTestError(f"{path} is from a run of {data['count']} shards, not {count}")
        else:
            finished.add(data["index"])
    return reports, sorted(set(range(count)) - finished)


def render_missing_shards(missing: Sequence[int], count: int) -> str:
    names = ", ".join(str(index) for index in missing)
    noun = "shard" if len(missing) == 1 else "shards"
    return (
        f"**Incomplete:** {noun} {names} of {count} did not finish; "
        "their tests are missing below.\n"
    )


def group_title(position: int, total: int, label: str) -> str:
    return f"::group::[{position}/{total}] {label}"


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
        ["bazel", "query", "--keep_going", "--output=label", "--", expression],
        root,
        capture_output=True,
    )
    if result.returncode not in (0, 3):
        raise MutationTestError(f"bazel query failed:\n{result.stderr}")
    return [line for line in result.stdout.splitlines() if line.startswith("//")]


def git_changed_files(root: Path, diff_ref: str) -> list[str]:
    result = _run(
        ["git", "diff", "--name-only", "--diff-filter=d", "--end-of-options", f"{diff_ref}...HEAD"],
        root,
        capture_output=True,
    )
    if result.returncode:
        raise MutationTestError(f"git diff against {diff_ref} failed:\n{result.stderr}")
    return result.stdout.splitlines()


def mutation_flags(plugin: str, config_path: Path) -> list[str]:
    """Flags for the mutated build; cquery reuses them to see the same targets."""
    return [
        "--config=mutation",
        f"--copt=-fpass-plugin={plugin}",
        f"--action_env=MULL_CONFIG={config_path}",
    ]


def build_tests(root: Path, targets: Sequence[str], flags: Sequence[str]) -> None:
    result = _run(["bazel", "build", *flags, *targets], root)
    if result.returncode:
        raise MutationTestError("building the mutated tests failed")


def env_query_command(targets: Sequence[str], flags: Sequence[str]) -> list[str]:
    return [
        "bazel",
        "cquery",
        *flags,
        "--output=starlark",
        f"--starlark:expr={TEST_ENV_EXPR}",
        "--",
        f"set({' '.join(targets)})",
    ]


def parse_test_environments(output: str) -> dict[str, dict[str, str]]:
    """Map each test label to its `env`, from TEST_ENV_EXPR's one-object lines."""
    environments: dict[str, dict[str, str]] = {}
    for line in output.splitlines():
        try:
            entry = json.loads(line)
        except json.JSONDecodeError as error:
            raise MutationTestError(f"unexpected cquery output line {line!r}") from error
        environments[entry["label"].removeprefix("@@")] = entry["env"]
    return environments


def query_test_environments(
    root: Path, targets: Sequence[str], flags: Sequence[str]
) -> dict[str, dict[str, str]]:
    result = _run(env_query_command(targets, flags), root, capture_output=True)
    if result.returncode:
        raise MutationTestError(f"bazel cquery for the test environments failed:\n{result.stderr}")
    return parse_test_environments(result.stdout)


def runner_environment(
    base: Mapping[str, str], test_env: Mapping[str, str], config_path: Path
) -> dict[str, str]:
    return {**base, **test_env, "MULL_CONFIG": str(config_path)}


def runner_command(runner: str, binary: Path, report_dir: Path) -> list[str]:
    return [
        runner,
        "--reporters",
        "Elements",
        "--report-dir",
        str(report_dir),
        "--report-name",
        binary.name,
        "--timeout",
        str(ORIGINAL_RUN_TIMEOUT_MS),
        # The runner otherwise fails below a 100% score; survivors are
        # reported, never fatal (ADR 0022).
        "--allow-surviving",
        "--no-test-output",
        "--no-mutant-output",
        str(binary),
    ]


def run_mull(
    root: Path,
    runner: str,
    label: str,
    test_env: Mapping[str, str],
    config_path: Path,
    report_dir: Path,
) -> Mapping | None:
    binary = binary_path(root, label)
    # The test's env holds runfiles-relative paths, as under `bazel test`.
    runfiles = binary.parent / f"{binary.name}.runfiles" / "_main"
    result = _run(
        runner_command(runner, binary, report_dir),
        runfiles if runfiles.is_dir() else root,
        env=runner_environment(os.environ, test_env, config_path),
    )
    if result.returncode:
        raise MutationTestError(f"mull-runner failed on {label}")
    report = report_dir / f"{binary.name}.json"
    # The runner writes no report when the git diff filter left nothing to run.
    return json.loads(report.read_text()) if report.exists() else None


def operand(value: str) -> str:
    """An argument passed on to git or Bazel as an operand, never as an option."""
    if not value or value.startswith("-"):
        raise argparse.ArgumentTypeError(f"{value!r} must not be empty or start with '-'")
    return value


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--diff-ref", type=operand, help="mutate only lines changed since this git ref"
    )
    parser.add_argument("--report-dir", type=Path, help="keep per-binary reports here")
    parser.add_argument("--summary", type=Path, help="append the Markdown summary to this file")
    parser.add_argument("--annotate", action="store_true", help="print GitHub annotations")
    parser.add_argument(
        "--scope",
        type=operand,
        default=PORTABLE_SCOPE,
        help=f"Bazel target pattern whose tests run (default: {PORTABLE_SCOPE})",
    )
    parser.add_argument(
        "--shard", type=parse_shard, help="run only shard I of N (0-based), e.g. 1/4"
    )
    parser.add_argument(
        "--merge-reports",
        type=Path,
        metavar="DIR",
        help="merge the shard report directories under DIR instead of running Mull",
    )
    parser.add_argument("--shards", type=int, help="with --merge-reports: how many shards ran")
    args = parser.parse_args(argv)
    if args.shard and not args.report_dir:
        parser.error("--shard needs --report-dir, where the merge job finds its reports")
    if args.shard and args.diff_ref:
        parser.error("--shard is for full and scoped runs; the changed-lines run is one job")
    if args.merge_reports and not (args.shards and args.shards > 0):
        parser.error("--merge-reports needs --shards N")
    if args.merge_reports and (args.shard or args.diff_ref):
        parser.error("--merge-reports runs no tests; drop --shard and --diff-ref")
    return args


def _emit(summary: str, merged: Mapping[MutantKey, str], args: argparse.Namespace) -> None:
    print(summary)
    if args.summary:
        with args.summary.open("a") as handle:
            handle.write(summary)
    if args.annotate:
        for line in render_annotations(merged):
            print(line)


def merge_shards(args: argparse.Namespace, root: Path) -> int:
    reports, missing = collect_shard_reports(args.merge_reports, args.shards)
    merged = merge_reports(reports, root)
    summary = render_summary(merged, "Mutation testing")
    if missing:
        heading, _, body = summary.partition("\n\n")
        summary = f"{heading}\n\n{render_missing_shards(missing, args.shards)}\n{body}"
    _emit(summary, merged, args)
    if missing:
        names = ", ".join(str(index) for index in missing)
        raise MutationTestError(f"shard(s) {names} of {args.shards} left no manifest")
    return 0


def main(argv: Sequence[str] = sys.argv[1:]) -> int:
    args = _parse_args(argv)
    root = Path(__file__).resolve().parent.parent
    base_config = (root / CONFIG_NAME).read_text()
    title = (
        f"Mutation testing (lines changed since {args.diff_ref})"
        if args.diff_ref
        else "Mutation testing"
    )
    try:
        if args.merge_reports:
            return merge_shards(args, root)
        changed = None
        if args.diff_ref:
            changed = changed_portable_sources(git_changed_files(root, args.diff_ref))
            if not changed:
                _emit(f"## {title}\n\nNo portable sources changed.\n", {}, args)
                return 0
        runner, plugin = find_mull(os.environ)
        targets = query_tests(root, args.scope, changed)
        if args.shard:
            targets = args.shard.select(targets)
            if not targets:
                args.report_dir.mkdir(parents=True, exist_ok=True)
                write_shard_manifest(args.report_dir, args.shard, targets)
                _emit(f"## {title}\n\nNo tests fall in this shard.\n", {}, args)
                return 0
        if not targets:
            _emit(f"## {title}\n\nNo portable tests depend on the change.\n", {}, args)
            return 0
        with tempfile.TemporaryDirectory(prefix="fastecu-mull-") as scratch:
            report_dir = args.report_dir or Path(scratch) / "reports"
            report_dir.mkdir(parents=True, exist_ok=True)
            config_path = root / CONFIG_NAME
            if args.diff_ref:
                config_path = Path(scratch) / CONFIG_NAME
                config_path.write_text(diff_config(base_config, args.diff_ref, root))
            # The build always uses the checked-in config, so every mode shares one
            # set of mutated binaries; the runner applies the diff filter.
            flags = mutation_flags(plugin, root / CONFIG_NAME)
            build_tests(root, targets, flags)
            environments = query_test_environments(root, targets, flags)
            missing = sorted(set(targets) - environments.keys())
            if missing:
                raise MutationTestError(f"cquery reported no environment for {', '.join(missing)}")
            grouped = os.environ.get("GITHUB_ACTIONS") == "true"
            reports = []
            for position, label in enumerate(targets, start=1):
                if grouped:
                    print(group_title(position, len(targets), label), flush=True)
                try:
                    report = run_mull(
                        root, runner, label, environments[label], config_path, report_dir
                    )
                finally:
                    if grouped:
                        print("::endgroup::", flush=True)
                if report:
                    reports.append(report)
            if args.shard:
                write_shard_manifest(report_dir, args.shard, targets)
        merged = merge_reports(reports, root)
        _emit(render_summary(merged, title), merged, args)
        return 0
    except MutationTestError as error:
        print(f"mutation_test: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
