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
  --shard I/N      one duration-balanced slice of the selected work;
                   writes reports and a manifest for --merge-reports.
  --schedule FILE  duration estimates and GoogleTest splits (optional).
  --merge-reports  merge the reports of N shards' report directories.

Surviving mutants never fail the run (mutation testing is advisory); a tool or
build failure does. Exit codes: 0 = ran (or nothing to do), 2 = tool failure.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path, PurePath

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
# coverage build. Mutant runs get 10x the baseline time, with a one-second
# floor: Mull's 30 ms default misclassifies fast assertion failures under CI
# scheduling noise as timeouts.
ORIGINAL_RUN_TIMEOUT_MS = 60_000
MINIMUM_MUTANT_TIMEOUT_MS = 1_000
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
PERFORMANCE_REPORT = "_performance.json"


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


@dataclasses.dataclass(frozen=True)
class WorkItem:
    label: str
    index: int = 0
    count: int = 1
    seconds: float = 1.0

    @property
    def id(self) -> str:
        return self.label if self.count == 1 else f"{self.label}[{self.index}/{self.count}]"

    def report_path(self, directory: Path) -> Path:
        return (
            directory
            / self.label.removeprefix("//").replace(":", "/")
            / f"gtest-{self.index}-of-{self.count}"
        )

    def environment(self) -> dict[str, str]:
        if self.count == 1:
            return {}
        return {"GTEST_TOTAL_SHARDS": str(self.count), "GTEST_SHARD_INDEX": str(self.index)}


def plan_work(labels: Iterable[str], schedule: Mapping) -> list[WorkItem]:
    """Expand only selected labels; stale estimates cannot remove or add tests."""
    work = []
    for label in sorted(set(labels)):
        entry = schedule.get(label, {})
        seconds = entry.get("seconds", 1.0)
        count = entry.get("test_shards", 1)
        if not isinstance(seconds, (float, int)) or not math.isfinite(seconds) or seconds <= 0:
            raise MutationTestError(f"invalid duration estimate for {label}: {seconds!r}")
        if type(count) is not int or count < 1:
            raise MutationTestError(f"invalid GoogleTest shard count for {label}: {count!r}")
        costs = entry.get("shard_seconds", [seconds / count] * count)
        if len(costs) != count or any(
            not isinstance(cost, (float, int)) or not math.isfinite(cost) or cost <= 0
            for cost in costs
        ):
            raise MutationTestError(f"invalid per-slice duration estimates for {label}")
        work.extend(WorkItem(label, index, count, costs[index]) for index in range(count))
    return work


def balance_work(work: Sequence[WorkItem], shard: Shard) -> list[WorkItem]:
    """Longest estimated work first, with stable ties and no planning job."""
    bins: list[list[WorkItem]] = [[] for _ in range(shard.count)]
    loads = [0.0] * shard.count
    for item in sorted(work, key=lambda item: (-item.seconds, item.id)):
        index = min(range(shard.count), key=lambda index: (loads[index], index))
        bins[index].append(item)
        loads[index] += item.seconds
    return bins[shard.index]


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


def relative_source_path(path: str, root: PurePath) -> str:
    """Map a path recorded in debug info to a workspace-relative one."""
    # Reports can contain native Windows paths; git identities use POSIX separators.
    path = path.replace("\\", "/")
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
        f"Of the killed mutants, "
        f"{sum(status == 'Timeout' for status in merged.values())} timed out.",
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


def write_shard_manifest(
    report_dir: Path, shard: Shard, labels: Sequence[str], *, selection: Sequence[str] | None = None
) -> None:
    manifest = {"index": shard.index, "count": shard.count, "labels": list(labels)}
    if selection is not None:
        manifest["selection"] = list(selection)
    (report_dir / SHARD_MANIFEST).write_text(json.dumps(manifest))


def collect_shard_reports(directory: Path, count: int) -> tuple[list[Mapping], list[int]]:
    """Every report under `directory`, and the shards that left no manifest."""
    reports, finished, manifests = [], set(), []
    for path in sorted(directory.rglob("*.json")):
        if path.name == PERFORMANCE_REPORT:
            continue
        data = json.loads(path.read_text())
        if path.name != SHARD_MANIFEST:
            reports.append(data)
        elif data["count"] != count:
            raise MutationTestError(f"{path} is from a run of {data['count']} shards, not {count}")
        else:
            if data["index"] in finished or data["index"] not in range(count):
                raise MutationTestError(f"duplicate or invalid shard index in {path}")
            finished.add(data["index"])
            manifests.append(data)
    if len(finished) == count:
        validate_selection(manifests)
    return reports, sorted(set(range(count)) - finished)


def validate_selection(manifests: Sequence[Mapping]) -> None:
    if not any("selection" in manifest for manifest in manifests):
        return
    expected = manifests[0].get("selection")
    labels = [label for manifest in manifests for label in manifest["labels"]]
    if (
        any(manifest.get("selection") != expected for manifest in manifests)
        or len(labels) != len(set(labels))
        or set(labels) != set(expected or [])
    ):
        raise MutationTestError("shard manifests contain missing, duplicated or inconsistent work")


def render_performance(reports: Sequence[Mapping]) -> str:
    binaries = [binary for report in reports for binary in report["binaries"]]
    total = sum(binary["seconds"] for binary in binaries)
    lines = [
        "### Performance",
        "",
        f"Aggregate Mull execution: **{total:.1f}s**.",
        "",
        "| Work item | Seconds | Status |",
        "| --- | ---: | --- |",
    ]
    for binary in sorted(binaries, key=lambda binary: (-binary["seconds"], binary["id"]))[:15]:
        lines.append(f"| `{binary['id']}` | {binary['seconds']:.1f} | {binary['status']} |")
    lines += [
        "",
        "| Shard | Build seconds | Query seconds | Mull seconds |",
        "| --- | ---: | ---: | ---: |",
    ]
    for index, report in enumerate(reports):
        duration = sum(binary["seconds"] for binary in report["binaries"])
        lines.append(
            f"| {report.get('shard', index)} | {report['build_seconds']:.1f} | "
            f"{report['query_seconds']:.1f} | {duration:.1f} |"
        )
    return "\n".join(lines) + "\n"


def render_comparison(current: Mapping[MutantKey, str], baseline: Mapping[MutantKey, str]) -> str:
    missing = baseline.keys() - current.keys()
    added = current.keys() - baseline.keys()
    lost = [
        key
        for key, status in baseline.items()
        if status == "Killed" and current.get(key) != "Killed"
    ]
    timeouts = [
        key
        for key, status in current.items()
        if status == "Timeout" and baseline.get(key) != "Timeout"
    ]
    survivors = [
        key
        for key, status in current.items()
        if status == "Survived" and baseline.get(key) != "Survived"
    ]
    return (
        "### Baseline comparison\n\n"
        f"Mutant identities: **{len(missing)} missing, {len(added)} added**.\n\n"
        f"**{len(lost)} lost direct kills; {len(timeouts)} new timeouts**. "
        f"Additional survivors: **{len(survivors)}**. "
        "Review changes before retaining a speedup; "
        "compare identical production sources and scope.\n"
    )


def read_reports(directory: Path) -> list[Mapping]:
    return [
        json.loads(path.read_text())
        for path in sorted(directory.rglob("*.json"))
        if path.name not in (SHARD_MANIFEST, PERFORMANCE_REPORT)
    ]


def read_performance(directory: Path) -> list[Mapping]:
    return [json.loads(path.read_text()) for path in sorted(directory.rglob(PERFORMANCE_REPORT))]


def refresh_schedule(schedule: Mapping, reports: Sequence[Mapping]) -> dict:
    """Refresh measured slices only, refusing partial or failed binary runs."""
    tests = dict(schedule["tests"])
    grouped: dict[str, list[Mapping]] = {}
    for report in reports:
        for binary in report["binaries"]:
            grouped.setdefault(binary["label"], []).append(binary)
    if not grouped:
        raise MutationTestError("no performance records to refresh")
    for label, binaries in grouped.items():
        count = binaries[0]["test_shards"]
        if any(
            binary["status"] != "complete" or binary["test_shards"] != count for binary in binaries
        ) or sorted(binary["test_shard"] for binary in binaries) != list(range(count)):
            raise MutationTestError(f"incomplete or duplicated timing slices for {label}")
        costs = [
            max(0.001, round(binary["seconds"], 3))
            for binary in sorted(binaries, key=lambda binary: binary["test_shard"])
        ]
        tests[label] = {"seconds": round(sum(costs), 3)}
        if count > 1:
            tests[label].update(test_shards=count, shard_seconds=costs)
    return {
        **schedule,
        "timing_runs": sorted({report["run_id"] for report in reports if report.get("run_id")}),
        "tests": dict(sorted(tests.items())),
    }


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


def mutation_flags(plugin: str, config_path: Path, platform: str = sys.platform) -> list[str]:
    """Flags for the mutated build; cquery reuses them to see the same targets.

    macOS also gets `--config=mutation-macos`: Bazel's default Apple toolchain
    compiles with Xcode's clang, which cannot load Mull's plugin.
    """
    return [
        "--config=mutation",
        *(["--config=mutation-macos"] if platform == "darwin" else []),
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
        "--minimum-timeout",
        str(MINIMUM_MUTANT_TIMEOUT_MS),
        # The runner otherwise fails below a 100% score; survivors are
        # reported, never fatal (ADR 0022).
        "--allow-surviving",
        "--no-test-output",
        "--no-mutant-output",
        str(binary),
        # Once an assertion kills a mutant, later cases cannot improve its
        # verdict. Passing warm-up/baseline runs still execute every case.
        "--gtest_fail_fast",
    ]


def run_mull(
    root: Path,
    runner: str,
    label: str,
    test_env: Mapping[str, str],
    config_path: Path,
    report_dir: Path,
    item: WorkItem | None = None,
) -> Mapping | None:
    binary = binary_path(root, label)
    # The test's env holds runfiles-relative paths, as under `bazel test`.
    runfiles = binary.parent / f"{binary.name}.runfiles" / "_main"
    report_dir.mkdir(parents=True, exist_ok=True)
    environment = runner_environment(os.environ, test_env, config_path)
    # Prevent a caller's Bazel/GoogleTest sharding state from dropping cases.
    for name in ("GTEST_TOTAL_SHARDS", "GTEST_SHARD_INDEX", "GTEST_SHARD_STATUS_FILE"):
        environment.pop(name, None)
    if item is not None:
        environment.update(item.environment())
    result = _run(
        runner_command(runner, binary, report_dir),
        runfiles if runfiles.is_dir() else root,
        env=environment,
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


# One or more target patterns joined by `+` or `-`: no query functions or
# parentheses, so `--scope` cannot rewrite the query it is spliced into.
_TARGET_PATTERN = re.compile(r"//[\w./:-]*(?: [+-] //[\w./:-]*)*")


def target_pattern(value: str) -> str:
    """A Bazel target pattern expression, safe to splice into a query."""
    if not _TARGET_PATTERN.fullmatch(value):
        raise argparse.ArgumentTypeError(
            f"{value!r} is not a target pattern such as //src/a/... + //src/b/..."
        )
    return value


def report_directory(path: Path, root: Path, environ: Mapping[str, str]) -> Path:
    """`path` resolved, provided it lies in the checkout or a temp directory."""
    resolved = (root / path).resolve()
    bases = [root.resolve(), Path(tempfile.gettempdir()).resolve()]
    if environ.get("RUNNER_TEMP"):
        bases.append(Path(environ["RUNNER_TEMP"]).resolve())
    if not any(resolved.is_relative_to(base) for base in bases):
        raise MutationTestError(
            f"{path} is outside the checkout and the temp directories; reports go there"
        )
    return resolved


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
        type=target_pattern,
        default=PORTABLE_SCOPE,
        help=f"Bazel target pattern whose tests run (default: {PORTABLE_SCOPE})",
    )
    parser.add_argument(
        "--shard", type=parse_shard, help="run only balanced shard I of N (0-based), e.g. 1/4"
    )
    parser.add_argument(
        "--schedule", type=Path, help="JSON duration estimates and GoogleTest splits"
    )
    parser.add_argument(
        "--refresh-schedule",
        type=Path,
        metavar="REPORT_DIR",
        help="update --schedule from complete per-slice performance records",
    )
    parser.add_argument(
        "--baseline-reports",
        type=Path,
        help="compare merged mutants with this baseline report directory",
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
    if args.refresh_schedule and (
        not args.schedule or args.merge_reports or args.shard or args.diff_ref
    ):
        parser.error("--refresh-schedule needs --schedule and runs no tests")
    if args.baseline_reports and not args.merge_reports:
        parser.error("--baseline-reports needs --merge-reports")
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
    performance = read_performance(args.merge_reports)
    if performance:
        summary += "\n" + render_performance(performance)
    if args.baseline_reports:
        baseline_reports = read_reports(args.baseline_reports)
        if not baseline_reports:
            raise MutationTestError("baseline directory contains no mutation reports")
        summary += "\n" + render_comparison(merged, merge_reports(baseline_reports, root))
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
    title = (
        f"Mutation testing (lines changed since {args.diff_ref})"
        if args.diff_ref
        else "Mutation testing"
    )
    try:
        if args.report_dir:
            args.report_dir = report_directory(args.report_dir, root, os.environ)
        schedule_path = report_directory(args.schedule, root, os.environ) if args.schedule else None
        if args.refresh_schedule:
            args.refresh_schedule = report_directory(args.refresh_schedule, root, os.environ)
        if args.baseline_reports:
            args.baseline_reports = report_directory(args.baseline_reports, root, os.environ)
        if args.refresh_schedule:
            manifests = sorted(args.refresh_schedule.rglob(SHARD_MANIFEST))
            if not manifests:
                raise MutationTestError("cannot refresh from an incomplete run: no shard manifests")
            count = json.loads(manifests[0].read_text())["count"]
            _, missing = collect_shard_reports(args.refresh_schedule, count)
            if missing:
                raise MutationTestError(
                    f"cannot refresh from an incomplete run: missing shards {missing}"
                )
            schedule = json.loads(schedule_path.read_text())
            refreshed = refresh_schedule(schedule, read_performance(args.refresh_schedule))
            schedule_path.write_text(json.dumps(refreshed, indent=2) + "\n")
            return 0
        if args.merge_reports:
            args.merge_reports = report_directory(args.merge_reports, root, os.environ)
            return merge_shards(args, root)
        if args.report_dir and args.report_dir.exists() and any(args.report_dir.iterdir()):
            raise MutationTestError("report directory must be empty; use a fresh path for each run")
        changed = None
        if args.diff_ref:
            changed = changed_portable_sources(git_changed_files(root, args.diff_ref))
            if not changed:
                _emit(f"## {title}\n\nNo portable sources changed.\n", {}, args)
                return 0
        runner, plugin = find_mull(os.environ)
        targets = query_tests(root, args.scope, changed)
        schedule = json.loads(schedule_path.read_text())["tests"] if schedule_path else {}
        selection = plan_work(targets, schedule)
        work = balance_work(selection, args.shard or Shard(0, 1))
        targets = sorted({item.label for item in work})
        if not targets:
            if args.shard:
                args.report_dir.mkdir(parents=True, exist_ok=True)
                write_shard_manifest(
                    args.report_dir, args.shard, [], selection=[item.id for item in selection]
                )
            _emit(f"## {title}\n\nNo portable tests depend on the change.\n", {}, args)
            return 0
        with tempfile.TemporaryDirectory(prefix="fastecu-mull-") as scratch:
            report_dir = args.report_dir or Path(scratch) / "reports"
            report_dir.mkdir(parents=True, exist_ok=True)
            config_path = root / CONFIG_NAME
            if args.diff_ref:
                base_config = (root / CONFIG_NAME).read_text()
                config_path = Path(scratch) / CONFIG_NAME
                config_path.write_text(diff_config(base_config, args.diff_ref, root))
            # The build always uses the checked-in config, so every mode shares one
            # set of mutated binaries; the runner applies the diff filter.
            flags = mutation_flags(plugin, root / CONFIG_NAME)
            performance = {
                "revision": os.environ.get("GITHUB_SHA", "local"),
                "run_id": os.environ.get("GITHUB_RUN_ID"),
                "shard": args.shard.index if args.shard else 0,
                "build_seconds": 0.0,
                "query_seconds": 0.0,
                "binaries": [],
            }
            started = time.monotonic()
            build_tests(root, targets, flags)
            performance["build_seconds"] = time.monotonic() - started
            started = time.monotonic()
            environments = query_test_environments(root, targets, flags)
            performance["query_seconds"] = time.monotonic() - started
            missing = sorted(set(targets) - environments.keys())
            if missing:
                raise MutationTestError(f"cquery reported no environment for {', '.join(missing)}")
            grouped = os.environ.get("GITHUB_ACTIONS") == "true"
            reports = []
            for position, item in enumerate(work, start=1):
                if grouped:
                    print(group_title(position, len(work), item.id), flush=True)
                started = time.monotonic()
                timing = {
                    "id": item.id,
                    "label": item.label,
                    "test_shard": item.index,
                    "test_shards": item.count,
                    "status": "failed",
                    "seconds": 0.0,
                }
                try:
                    report = run_mull(
                        root,
                        runner,
                        item.label,
                        environments[item.label],
                        config_path,
                        item.report_path(report_dir),
                        item,
                    )
                    timing["status"] = "complete"
                finally:
                    timing["seconds"] = time.monotonic() - started
                    performance["binaries"].append(timing)
                    (report_dir / PERFORMANCE_REPORT).write_text(
                        json.dumps(performance, indent=2) + "\n"
                    )
                    if grouped:
                        print("::endgroup::", flush=True)
                if report:
                    reports.append(report)
            if args.shard:
                write_shard_manifest(
                    report_dir,
                    args.shard,
                    [item.id for item in work],
                    selection=[item.id for item in selection],
                )
        merged = merge_reports(reports, root)
        _emit(
            render_summary(merged, title) + "\n" + render_performance([performance]), merged, args
        )
        return 0
    except MutationTestError as error:
        print(f"mutation_test: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
