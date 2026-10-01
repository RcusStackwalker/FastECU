"""Temporary, fail-closed evidence report for the Windows serial campaign."""

import argparse
import json
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import urlparse
from urllib.request import url2pathname


def report(root, runs, exit_code):
    problems = []
    targets = (root / "targets.txt").read_text(encoding="utf-8-sig").split()
    results = {label: [] for label in targets}
    if not targets:
        problems.append("empty target inventory")
    finished = False
    try:
        lines = (root / "events.jsonl").read_text(encoding="utf-8").splitlines()
    except OSError as error:
        lines = []
        problems.append(str(error))
    for line in lines:
        try:
            event = json.loads(line)
        except ValueError:
            problems.append("incomplete or malformed build event")
            continue
        if "finished" in event:
            finished = True
            # Protobuf JSON omits the zero-valued SUCCESS code.
            if (
                "exitCode" not in event["finished"]
                or event["finished"]["exitCode"].get("code", 0) != 0
            ):
                problems.append("Bazel build did not finish successfully")
        identity = event.get("id", {}).get("testResult")
        if identity is None:
            continue
        label = identity["label"]
        if label not in results:
            problems.append(f"unexpected test target: {label}")
            results[label] = []
        result = event["testResult"]
        run = identity.get("run", 0)
        attempt = identity.get("attempt", 0)
        shard = identity.get("shard", 0)
        cached = result.get("cachedLocally", False) or result.get("executionInfo", {}).get(
            "cachedRemotely", False
        )
        if cached:
            problems.append(f"cached result: {label} run {run}")
        if attempt != 1 or shard != 1:
            problems.append(f"unexpected retry or shard: {label} {identity}")
        destination = (
            root
            / "attempts"
            / label.removeprefix("//").replace(":", "/").lstrip("/")
            / f"run-{run}-attempt-{attempt}-shard-{shard}"
        )
        destination.mkdir(parents=True, exist_ok=True)
        skipped = []
        copied = set()
        for output in result.get("testActionOutput", []):
            name = output.get("name")
            if name not in ("test.log", "test.xml"):
                continue
            uri = urlparse(output.get("uri", ""))
            try:
                if uri.scheme != "file" or uri.netloc:
                    raise ValueError("expected a local file URI")
                target = destination / name
                shutil.copyfile(Path(url2pathname(uri.path)), target)
                copied.add(name)
                if name == "test.xml":
                    for case in ET.parse(target).iter("testcase"):
                        if case.find("skipped") is not None or case.get("status") == "notrun":
                            skipped.append(f"{case.get('classname', '')}.{case.get('name', '')}")
            except (OSError, ValueError, ET.ParseError) as error:
                problems.append(f"{label} run {run} {name}: {error}")
        if copied != {"test.log", "test.xml"}:
            problems.append(f"missing log/XML: {label} run {run}")
        results[label].append(
            {"run": run, "status": result.get("status", "UNKNOWN"), "skipped": skipped}
        )
    if not finished or exit_code != 0:
        problems.append(f"incomplete or failed invocation (exit {exit_code}, finished={finished})")
    failed = False
    rows = []
    skips = set()
    for label, attempts in sorted(results.items()):
        actual_runs = [attempt["run"] for attempt in attempts]
        if sorted(actual_runs) != list(range(1, runs + 1)):
            problems.append(f"missing, duplicate, or unexpected runs: {label}")
        passed = sum(attempt["status"] == "PASSED" for attempt in attempts)
        failures = len(attempts) - passed
        failed |= any(attempt["status"] == "FAILED" for attempt in attempts)
        for attempt in attempts:
            if attempt["status"] not in ("PASSED", "FAILED"):
                problems.append(f"{label} run {attempt['run']}: {attempt['status']}")
        missing = len(set(range(1, runs + 1)) - set(actual_runs))
        rows.append(f"| `{label}` | {len(attempts)} | {passed} | {failures} | {missing} |")
        for attempt in attempts:
            skips.update(f"{label}: {case}" for case in attempt["skipped"])
    outcome = (
        "failures observed"
        if failed
        else "inconclusive"
        if problems
        else "not reproduced in this campaign"
    )
    summary = [
        f"# Windows serial campaign: {outcome}",
        "",
        f"Required fresh executions per target: {runs}.",
        "",
        "| Target | Attempted | Passed | Failed | Missing |",
        "| --- | ---: | ---: | ---: | ---: |",
        *rows,
        "",
        "## Skipped test cases",
        "",
        *([f"- {skip}" for skip in sorted(skips)] or ["None observed in available XML."]),
        "",
        "## Evidence problems",
        "",
        *([f"- {problem}" for problem in problems] or ["None."]),
        "",
    ]
    (root / "summary.md").write_text("\n".join(summary), encoding="utf-8")
    (root / "results.json").write_text(
        json.dumps({"outcome": outcome, "targets": results, "problems": problems}, indent=2),
        encoding="utf-8",
    )
    print("\n".join(summary))
    return int(failed or bool(problems))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--runs", type=int, default=50)
    parser.add_argument("--exit-code", type=int, required=True)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    raise SystemExit(report(args.directory, args.runs, args.exit_code))
