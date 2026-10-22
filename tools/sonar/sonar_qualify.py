"""Collect legacy inputs and compare configured Sonar command semantics."""

import argparse
import json
import os
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

import sonar_commands as commands


def _relative(value: str, root: str) -> str:
    if root and value.startswith(root.rstrip("/") + "/"):
        return value[len(root.rstrip("/")) + 1 :]
    return value


def _argument(value: str, root: str) -> str:
    for prefix in ("-I", "-F", "-iquote", "-isystem", "--sysroot="):
        if value.startswith(prefix):
            return prefix + _relative(value[len(prefix) :], root)
    return _relative(value, root)


def _index(records):
    result = {}
    for record in records:
        root = record.get("directory", "")
        identity = (
            record["owner"].removeprefix("@@"),
            record["configuration"],
            _relative(record["file"], root),
            tuple(_relative(path, root) for path in record["outputs"]),
        )
        arguments = [_argument(argument, root) for argument in record["arguments"]]
        if identity in result and result[identity] != arguments:
            raise ValueError(f"conflicting configured identity: {identity}")
        result[identity] = arguments
    if not result:
        raise ValueError("empty qualification command scope")
    return result


def compare_commands(baseline, candidate) -> dict:
    """Report all scope and ordered-argument differences; never sort arguments."""
    before, after = _index(baseline), _index(candidate)
    return {
        "missing": sorted(before.keys() - after.keys()),
        "extra": sorted(after.keys() - before.keys()),
        "arguments": [
            {"identity": identity, "baseline": before[identity], "candidate": after[identity]}
            for identity in sorted(before.keys() & after.keys())
            if before[identity] != after[identity]
        ],
    }


def _artifact_paths(query: dict) -> dict:
    fragments = {fragment["id"]: fragment for fragment in query["pathFragments"]}

    def path(identifier):
        parts = []
        visited = set()
        while identifier:
            if identifier in visited:
                raise ValueError("cyclic aquery path fragments")
            visited.add(identifier)
            fragment = fragments[identifier]
            parts.append(fragment["label"])
            identifier = fragment.get("parentId")
        return "/".join(reversed(parts))

    return {artifact["id"]: path(artifact["pathFragmentId"]) for artifact in query["artifacts"]}


def records_from_aquery(query: dict, execroot: Path) -> list[dict]:
    """Recover configured identities from the reference action graph, not filenames."""
    targets = {target["id"]: target["label"] for target in query["targets"]}
    artifacts = _artifact_paths(query)
    records = []
    for action in query["actions"]:
        owner = targets[action["targetId"]]
        if action["mnemonic"] != "CppCompile" or not owner.startswith(("//", "@//")):
            continue
        argv = action["arguments"]
        if any(argument.startswith("@") for argument in argv) or "-c" not in argv:
            raise ValueError(f"unexpanded/unsupported baseline command: {owner}")
        outputs = sorted(artifacts[identifier] for identifier in action["outputIds"])
        if not outputs or not outputs[0].startswith("bazel-out/"):
            raise ValueError(f"unsupported baseline output identity: {owner}")
        records.append(
            {
                "owner": owner.removeprefix("@"),
                "configuration": "/".join(outputs[0].split("/")[:3]),
                "file": argv[argv.index("-c") + 1],
                "arguments": argv,
                "inputs": [],
                "outputs": outputs,
                "directory": str(execroot),
            }
        )
    if not records:
        raise ValueError("empty baseline compile graph")
    return records


def _requested_inputs(records, by_path):
    pending = []
    for record in records:
        for original in record["inputs"]:
            path = _relative(original, record.get("directory", ""))
            if path in by_path:
                pending.append(by_path[path])
            elif path.startswith("bazel-out/"):
                raise ValueError(f"missing generated input in action graph: {path}")
    return pending


def _dependency_artifacts(action, sets, visited):
    pending = list(action.get("inputDepSetIds", []))
    artifacts = []
    while pending:
        dependency = pending.pop()
        if dependency in visited:
            continue
        visited.add(dependency)
        value = sets[dependency]
        artifacts.extend(value.get("directArtifactIds", []))
        pending.extend(value.get("transitiveDepSetIds", []))
    return artifacts


def _compile_tool(action, owner, configuration):
    if action["mnemonic"] not in ("CppCompile", "CppLink"):
        return None
    if owner.startswith(("//", "@//", "@@//")) and not configuration.get("isTool", False):
        raise ValueError(f"application compile/link required by Sonar inputs: {owner}")
    return owner


def verify_input_closure(query: dict, records: list[dict]) -> dict:
    """Audit producer dependencies independent of whether actions hit a cache."""
    artifacts = _artifact_paths(query)
    by_path = {path: identifier for identifier, path in artifacts.items()}
    producers = {
        output: index
        for index, action in enumerate(query["actions"])
        for output in action.get("outputIds", [])
    }
    owners = {target["id"]: target["label"] for target in query["targets"]}
    configurations = {value["id"]: value for value in query.get("configuration", [])}
    sets = {value["id"]: value for value in query.get("depSetOfFiles", [])}
    pending = _requested_inputs(records, by_path)
    visited_files, visited_actions, visited_sets = set(), set(), set()
    tools = set()
    while pending:
        identifier = pending.pop()
        if identifier in visited_files:
            continue
        visited_files.add(identifier)
        index = producers.get(identifier)
        if index is None:
            if artifacts[identifier].startswith("bazel-out/"):
                raise ValueError(f"missing producer for generated input: {artifacts[identifier]}")
            continue
        if index in visited_actions:
            continue
        visited_actions.add(index)
        action = query["actions"][index]
        owner = owners[action["targetId"]]
        tool = _compile_tool(action, owner, configurations.get(action.get("configurationId"), {}))
        if tool:
            tools.add(tool)
        pending.extend(_dependency_artifacts(action, sets, visited_sets))
    return {"visited_actions": len(visited_actions), "tool_compiles": sorted(tools)}


def _database_index(entries):
    result = Counter()
    for entry in entries:
        root = entry["directory"]
        argv = [
            _argument(argument, root)
            for argument in entry["arguments"]
            if not argument.startswith("-fdebug-prefix-map=")
        ]
        # Hedron emits 'clang' for Apple's driver; the candidate resolves that
        # same selected driver with xcrun. Do not merge different driver names.
        if Path(argv[0]).name in ("clang", "clang++"):
            argv[0] = Path(argv[0]).name
        for index, argument in enumerate(argv):
            if index and argv[index - 1] == "-isysroot" and Path(argument).is_absolute():
                argv[index] = str(Path(argument).resolve())
        result[(_relative(entry["file"], root), tuple(argv))] += 1
    return result


def collect_baseline(destination: Path) -> None:
    workspace = Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY", Path.cwd()))
    destination.mkdir(parents=True, exist_ok=False)
    subprocess.run(
        [
            "bazel",
            "run",
            "--config=coverage",
            "//bazel/compile_commands:refresh",
            "--",
            "--config=coverage",
        ],
        cwd=workspace,
        check=True,
    )
    query = subprocess.run(
        [
            "bazel",
            "aquery",
            *commands.EXTRACTION_FLAGS,
            "--output=jsonproto",
            "--include_commandline",
            "mnemonic('CppCompile', filter('^(//|@//)', deps(//...)))",
        ],
        cwd=workspace,
        capture_output=True,
        text=True,
        check=True,
    )
    root = subprocess.run(
        ["bazel", "info", "execution_root"],
        cwd=workspace,
        capture_output=True,
        text=True,
        check=True,
    )
    records = records_from_aquery(json.loads(query.stdout), Path(root.stdout.strip()))
    (destination / "records.json").write_text(json.dumps(records, indent=2) + "\n")
    shutil.copyfile(workspace / "compile_commands.json", destination / "compile_commands.json")
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=workspace, capture_output=True, text=True, check=True
    )
    (destination / "context.json").write_text(
        json.dumps(
            {
                "commit": commit.stdout.strip(),
                "execroot": root.stdout.strip(),
                "extraction_flags": commands.EXTRACTION_FLAGS,
            },
            indent=2,
        )
        + "\n"
    )


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--collect-baseline", action="store_true")
    parser.add_argument("--check-input-closure", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    workspace = Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY", Path.cwd()))
    for name in ("baseline", "candidate", "output"):
        value = getattr(args, name)
        if value is not None and not value.is_absolute():
            setattr(args, name, workspace / value)
    try:
        if args.collect_baseline:
            collect_baseline(args.output)
            return 0
        if args.baseline is None or args.candidate is None:
            parser.error("comparison requires --baseline and --candidate")
        before = json.loads((args.baseline / "records.json").read_text())
        after = json.loads((args.candidate / "records.json").read_text())
        difference = compare_commands(before, after)
        before_db = _database_index(
            json.loads((args.baseline / "compile_commands.json").read_text())
        )
        after_db = _database_index(
            json.loads((args.candidate / "compile_commands.json").read_text())
        )
        database = {
            "missing": list((before_db - after_db).elements()),
            "extra": list((after_db - before_db).elements()),
        }
        args.output.mkdir(parents=True, exist_ok=False)
        report = {
            "configured": difference,
            "database": database,
            "baseline_count": len(before),
            "candidate_count": len(after),
            "normalizations": [
                "execution-root prefixes in path arguments",
                "selected clang executable path",
                "Apple SDK symlink",
                "main repository canonical label spelling",
                "debug-only prefix maps omitted by Hedron",
            ],
        }
        if args.check_input_closure:
            context = json.loads((args.baseline / "context.json").read_text())
            revision = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=workspace,
                capture_output=True,
                text=True,
                check=True,
            )
            if context["commit"] != revision.stdout.strip():
                raise ValueError("baseline and candidate source revisions differ")
            query = subprocess.run(
                [
                    "bazel",
                    "aquery",
                    *commands.EXTRACTION_FLAGS,
                    "--output=jsonproto",
                    "deps(//...)",
                ],
                cwd=workspace,
                capture_output=True,
                text=True,
                check=True,
            )
            report["input_closure"] = verify_input_closure(json.loads(query.stdout), after)
        (args.output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
        passed = not any(difference.values()) and not any(database.values())
        print(
            f"Sonar parity: {len(before)}/{len(after)} configured commands; "
            f"{'PASS' if passed else 'FAIL'}"
        )
        return 0 if passed else 1
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        print(f"Sonar qualification: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
