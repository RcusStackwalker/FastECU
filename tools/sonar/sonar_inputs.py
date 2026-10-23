"""Prepare complete Sonar compiler inputs without compiling application code."""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import unquote, urlparse

import sonar_commands as commands
import sonar_compdb_check

GROUPS = ("sonar_commands", "sonar_inputs")
ASPECT = "//tools/sonar:compile_commands.bzl%sonar_compile_commands"


class InputsError(ValueError):
    """Failed or incomplete preparation; no database may be consumed."""


@dataclass(frozen=True)
class BuildOutputs:
    execroot: Path
    groups: dict[str, list[Path]]


def _file_path(uri: str) -> Path:
    parsed = urlparse(uri)
    if parsed.scheme != "file" or parsed.netloc not in ("", "localhost"):
        raise InputsError(f"output was not downloaded to a local file: {uri}")
    path = Path(unquote(parsed.path))
    if not path.is_file() and not path.is_dir():
        raise InputsError(f"missing downloaded output: {path}")
    return path


def _event_groups(events: Path):
    sets = {}
    groups = []
    seen = set()
    announced = set()
    finished = False
    final = {}
    execroot = None
    for line in events.read_text().splitlines():
        final = json.loads(line)
        seen.add(json.dumps(final["id"], sort_keys=True))
        announced.update(json.dumps(child, sort_keys=True) for child in final.get("children", []))
        if "buildFinished" in final["id"]:
            status = final.get("finished", {}).get("exitCode")
            finished = isinstance(status, dict) and status.get("code", 0) == 0
        if "namedSetOfFiles" in final:
            sets[final["id"]["namedSet"]["id"]] = final["namedSetOfFiles"]
        if "workspaceInfo" in final:
            root = Path(final["workspaceInfo"]["localExecRoot"])
            if not root.is_absolute() or not root.is_dir() or execroot not in (None, root):
                raise InputsError("invalid or conflicting execution root in build events")
            execroot = root
        groups.extend(final.get("completed", {}).get("outputGroup", []))
    if not finished or not final.get("lastMessage") or announced - seen:
        raise InputsError("incomplete or unsuccessful build events: missing finish/events")
    if execroot is None:
        raise InputsError("missing execution root in build events")
    return execroot, sets, groups


def _group_paths(sets, groups, name):
    selected = [group for group in groups if group["name"] == name]
    if not selected or any(group.get("incomplete") for group in selected):
        raise InputsError(f"missing or incomplete output group: {name}")
    pending = [ref["id"] for group in selected for ref in group.get("fileSets", [])]
    visited = set()
    uris = set()
    while pending:
        identifier = pending.pop()
        if identifier in visited:
            continue
        visited.add(identifier)
        if identifier not in sets:
            raise InputsError(f"missing build event file set: {identifier}")
        value = sets[identifier]
        uris.update(file["uri"] for file in value.get("files", []))
        pending.extend(ref["id"] for ref in value.get("fileSets", []))
    if not uris:
        raise InputsError(f"empty output group: {name}")
    return sorted({_file_path(uri) for uri in uris})


def read_outputs(events: Path) -> BuildOutputs:
    """Resolve complete output groups by BEP identity, retaining configurations."""
    try:
        execroot, sets, groups = _event_groups(events)
        return BuildOutputs(execroot, {name: _group_paths(sets, groups, name) for name in GROUPS})
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise InputsError(f"invalid build events: {error}") from error


def apple_arguments(arguments: Sequence[str], *, run: Callable) -> list[str]:
    """Resolve Apple's wrapped-compiler placeholders for an external scanner."""
    if not any("__BAZEL_XCODE_" in argument for argument in arguments):
        return list(arguments)

    def value(command):
        result = run(command, capture_output=True, text=True, check=True)
        if not result.stdout.strip():
            raise InputsError(f"empty Apple toolchain response: {command}")
        return result.stdout.strip()

    developer = value(["xcode-select", "--print-path"])
    sdk = value(["xcrun", "--sdk", "macosx", "--show-sdk-path"])
    compiler = value(["xcrun", "--find", "clang"])
    replaced = [
        argument.replace("__BAZEL_XCODE_DEVELOPER_DIR__", developer).replace(
            "__BAZEL_XCODE_SDKROOT__", sdk
        )
        for argument in arguments
    ]
    if any("__BAZEL_XCODE_" in argument for argument in replaced):
        raise InputsError("unsupported Apple compiler placeholder")
    return [compiler, *replaced[1:]]


def _qt_header_resolves(entries):
    seen = set()
    for entry in entries:
        for include in sonar_compdb_check.include_dirs(entry["arguments"]):
            directory = sonar_compdb_check.resolve(entry["directory"], include)
            if directory not in seen:
                seen.add(directory)
                if (directory / "QtCore" / sonar_compdb_check.QT_MARKER).is_file():
                    return True
    return False


def _link_execroot(workspace: Path, execroot: Path) -> None:
    """Resolve execroot-relative arguments from the checkout, as Hedron's refresh does.

    The scanner keys its cache by the paths it analyzes; rooting the database in the
    checkout keeps sources at their repository paths instead of under the execroot.
    """
    if workspace.resolve() == execroot.resolve():
        return
    for name, target in (
        ("external", execroot.parent.parent / "external"),
        ("bazel-out", execroot / "bazel-out"),
    ):
        link = workspace / name
        if link.is_symlink():
            link.unlink()
        elif link.exists():
            raise InputsError(f"cannot link {name}: {link} exists and is not a symlink")
        if target.is_dir():
            link.symlink_to(target)
        elif name == "bazel-out":
            raise InputsError(f"execution root has no bazel-out tree: {target}")


def prepare(workspace: Path, destination: Path, *, run: Callable) -> Path:
    """Request metadata/input outputs, validate all of them, then publish once."""
    destination.mkdir(parents=True, exist_ok=True)
    database = destination / "compile_commands.json"
    database.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=destination) as temporary:
        events = Path(temporary) / "events.json"
        command = [
            "bazel",
            "build",
            *commands.EXTRACTION_FLAGS,
            f"--aspects={ASPECT}",
            "--output_groups=sonar_commands,sonar_inputs",
            "--remote_download_toplevel",
            "--nobuild_event_json_file_path_conversion",
            f"--build_event_json_file={events}",
            "//...",
        ]
        result = run(command, cwd=workspace, text=True)
        if result.returncode:
            raise InputsError(f"Sonar input build failed (exit {result.returncode})")
        outputs = read_outputs(events)
        execroot = outputs.execroot
        records = commands.load_fragments(outputs.groups["sonar_commands"])
        # Every declared input was checked by read_outputs, including symlink
        # targets. Check source references too, even for a malformed fragment
        # that names a source outside the declared input set.
        commands.validate_inputs(records, execroot)
        _link_execroot(workspace, execroot)
        entries = commands.database_entries(records, workspace)
        unresolved = sorted(e["file"] for e in entries if not (workspace / e["file"]).exists())
        if unresolved:
            raise InputsError(
                f"source files missing from the checkout view ({len(unresolved)}): "
                + ", ".join(unresolved[:10])
            )
        apple_results = {}

        def apple_run(command, **kwargs):
            key = tuple(command)
            if key not in apple_results:
                apple_results[key] = run(command, **kwargs)
            return apple_results[key]

        for entry in entries:
            entry["arguments"] = apple_arguments(entry["arguments"], run=apple_run)
        if not _qt_header_resolves(entries):
            raise InputsError("QtCore/qobjectdefs.h does not resolve through the database")
        pending = Path(temporary) / "compile_commands.json"
        pending.write_text(json.dumps(entries, indent=2) + "\n")
        pending.replace(database)
    print(f"Sonar inputs: {len(entries)} configured commands; {database}")
    return database


def main(argv: Sequence[str] | None = None, run: Callable = subprocess.run) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path("sonar-inputs"))
    args = parser.parse_args(argv)
    try:
        if sys.platform not in ("linux", "darwin"):
            raise InputsError("Sonar input preparation supports Linux and macOS")
        workspace = Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY", Path.cwd()))
        destination = (
            args.output_dir if args.output_dir.is_absolute() else workspace / args.output_dir
        )
        prepare(workspace, destination, run=run)
        return 0
    except (InputsError, commands.CommandError, OSError, subprocess.SubprocessError) as error:
        print(f"Sonar inputs: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
