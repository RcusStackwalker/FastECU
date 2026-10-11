#!/usr/bin/env python3
"""Measure the local full-scope cost of advisory inline clang-tidy checks."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
from contextlib import redirect_stdout
from datetime import UTC, datetime
from pathlib import Path

import clang_tidy_runner as runner


def fingerprint(workspace: Path) -> dict[str, str]:
    tree = runner._workspace_tree(workspace)
    inputs = {
        path: file
        for path, file in tree.files.items()
        if path.name == ".clang-tidy"
        or (
            path.parts[0] in {"src", "apps", "tests"}
            and path.suffix in runner.SOURCE_SUFFIXES | runner.HEADER_SUFFIXES
        )
        or path.as_posix()
        in {
            "scripts/clang_tidy_adapter.py",
            "scripts/clang_tidy_benchmark.py",
            "scripts/clang_tidy_runner.py",
            "scripts/clang_tidy_profile.py",
            "scripts/clang_tidy_scope.py",
        }
    }
    return {
        path.as_posix(): hashlib.sha256(file.read_bytes()).hexdigest()
        for path, file in sorted(inputs.items())
    }


def summarize(runs: list[dict]) -> dict:
    baseline = [run["phases"]["analysis"] for run in runs if not run["enabled"]]
    enabled = [
        run["phases"]["analysis"] + run["phases"]["custom-analysis"]
        for run in runs
        if run["enabled"]
    ]
    baseline_median = statistics.median(baseline)
    enabled_median = statistics.median(enabled)
    names = {name for run in runs for name in run["checks"] if name.startswith("custom-fastecu-")}
    return {
        "baseline_seconds": baseline,
        "enabled_seconds": enabled,
        "baseline_median_seconds": baseline_median,
        "enabled_median_seconds": enabled_median,
        "slowdown_percent": 100.0 * (enabled_median / baseline_median - 1.0),
        "custom_check_median_seconds": {
            name: statistics.median(run["checks"].get(name, 0.0) for run in runs if run["enabled"])
            for name in sorted(names)
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compdb-tool", required=True)
    parser.add_argument("--build-arg", action="append", default=[])
    parser.add_argument("--compdb-arg", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--output", type=Path, default=Path("reports/clang-tidy-custom"))
    args = parser.parse_args()
    root = runner.workspace_root(os.environ)
    output = args.output if args.output.is_absolute() else root / args.output
    output.mkdir(parents=True, exist_ok=False)
    compdb_tool = Path(args.compdb_tool).resolve()
    tools = runner.discover_tools("report", platform_name=sys.platform, environ=os.environ)
    metadata = {
        "host": platform.platform(),
        "machine": platform.machine(),
        "logical_cpus": os.cpu_count(),
        "jobs": args.jobs,
        "llvm": subprocess.check_output([tools.clang_tidy, "--version"], text=True).strip(),
        "revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True
        ).strip(),
        "working_tree": subprocess.check_output(["git", "status", "--short"], cwd=root, text=True),
        "root_configuration_sha256": hashlib.sha256(
            (root / ".clang-tidy").read_bytes()
        ).hexdigest(),
        "profiling_enabled_in_both_modes": True,
        "scope": "all translation units buildable on this host; included first-party headers",
        "input_sha256": fingerprint(root),
    }
    runs = []
    warmups = []
    database_digest = None
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    try:
        # Warm both modes, then alternate pair order to reduce ordering bias.
        for index, (warmup, enabled) in enumerate(
            [
                (True, False),
                (True, True),
                (False, False),
                (False, True),
                (False, True),
                (False, False),
                (False, False),
                (False, True),
            ]
        ):
            kind = "warmup" if warmup else "measured"
            mode = "enabled" if enabled else "baseline"
            name = f"{index + 1:02d}-{kind}-{mode}"
            print(f"Starting {name}; logs: {output / (name + '.log')}", flush=True)
            if fingerprint(root) != metadata["input_sha256"]:
                raise runner.WorkflowError(
                    "analysis configuration or implementation changed during benchmark"
                )
            started = datetime.now(UTC).isoformat()
            with (
                (output / (name + ".log")).open("w", encoding="utf-8", buffering=1) as log,
                redirect_stdout(log),
            ):
                report = runner.run_workflow(
                    mode="report",
                    workspace=root,
                    compdb_tool=str(compdb_tool),
                    platform_name=sys.platform,
                    environ=os.environ,
                    build_args=args.build_arg,
                    compdb_args=args.compdb_arg,
                    profile=True,
                    jobs=args.jobs,
                    custom_checks=enabled,
                    reuse_compdb=index != 0,
                )
            if report is None:
                raise runner.WorkflowError("benchmark analyzed no translation units")
            if enabled and not any(name.startswith("custom-fastecu-") for name in report["checks"]):
                raise runner.WorkflowError("inline per-check profile data is missing")
            if fingerprint(root) != metadata["input_sha256"]:
                raise runner.WorkflowError(
                    "analysis configuration or implementation changed during benchmark"
                )
            digest = hashlib.sha256((root / "compile_commands.json").read_bytes()).hexdigest()
            if database_digest is not None and digest != database_digest:
                raise runner.WorkflowError("compilation database changed during benchmark")
            database_digest = digest
            result = {
                "name": name,
                "enabled": enabled,
                "started_utc": started,
                "finished_utc": datetime.now(UTC).isoformat(),
                **report,
            }
            (output / (name + ".json")).write_text(json.dumps(result, indent=2) + "\n")
            (warmups if warmup else runs).append(result)
            print(f"Finished {name}: {report['phases']}", flush=True)
        summary = summarize(runs)
        document = {
            "metadata": {**metadata, "compilation_database_sha256": database_digest},
            "warmups": warmups,
            "runs": runs,
            "summary": summary,
        }
        (output / "results.json").write_text(json.dumps(document, indent=2) + "\n")
        print(json.dumps(summary, indent=2))
        return 0
    except runner.WorkflowError as error:
        print(f"clang-tidy benchmark: {error}; retained logs in {output}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
