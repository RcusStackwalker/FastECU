#!/usr/bin/env python3
"""Run clang-tidy from Bazel-derived compilation commands."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Sequence

import yaml

SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx"))


class WorkflowError(RuntimeError):
    """An actionable clang-tidy workflow failure."""


@dataclass(frozen=True)
class Tools:
    clang_tidy: str
    run_clang_tidy: str
    clang_apply_replacements: str | None = None


CommandRunner = Callable[..., subprocess.CompletedProcess[str]]


@dataclass(frozen=True)
class _Replacement:
    file_path: str
    offset: int
    length: int
    text: str


def workspace_root(environ: Mapping[str, str]) -> Path:
    value = environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not value:
        raise WorkflowError("run this command with bazel run from a Bazel workspace")
    root = Path(value).resolve()
    if not (root / "MODULE.bazel").is_file():
        raise WorkflowError(f"BUILD_WORKSPACE_DIRECTORY is not a Bazel workspace: {root}")
    return root


def load_project_entries(workspace: Path, database: Path) -> list[dict[str, object]]:
    if not database.is_file():
        raise WorkflowError(f"compilation database was not generated: {database}")
    try:
        value = json.loads(database.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise WorkflowError(f"compilation database is malformed: {error}") from error
    if not isinstance(value, list):
        raise WorkflowError("compilation database is malformed: expected a JSON list")

    root = workspace.resolve()
    entries: list[dict[str, object]] = []
    for entry in value:
        if not isinstance(entry, dict):
            raise WorkflowError("compilation database is malformed: entry is not an object")
        directory_value = entry.get("directory")
        file_value = entry.get("file")
        if not isinstance(directory_value, str) or not isinstance(file_value, str):
            raise WorkflowError("compilation database is malformed: entry lacks directory or file")

        directory = Path(directory_value)
        if not directory.is_absolute():
            directory = root / directory
        source = Path(file_value)
        if not source.is_absolute():
            source = directory / source
        source = source.resolve()
        try:
            source.relative_to(root)
        except ValueError:
            continue
        if source.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if not source.is_file():
            raise WorkflowError(f"missing workspace translation unit: {source}")
        entries.append(entry)

    if not entries:
        raise WorkflowError("compilation database contains no workspace translation units")
    return entries


def _search_directories(platform_name: str, environ: Mapping[str, str]) -> list[Path]:
    if platform_name == "darwin":
        return [
            Path("/opt/homebrew/opt/llvm/bin"),
            Path("/usr/local/opt/llvm/bin"),
        ]
    if platform_name.startswith("win"):
        return [
            Path(value) / "LLVM" / "bin"
            for key in ("ProgramFiles", "ProgramW6432")
            if (value := environ.get(key))
        ]
    return []


def find_executable(
    names: Sequence[str],
    *,
    platform_name: str,
    environ: Mapping[str, str],
) -> str:
    for name in names:
        located = shutil.which(name, path=environ.get("PATH"))
        if located:
            return located
    for directory in _search_directories(platform_name, environ):
        for name in names:
            candidates = [directory / name]
            if platform_name.startswith("win") and not Path(name).suffix:
                candidates.append(directory / f"{name}.exe")
            for candidate in candidates:
                if candidate.is_file():
                    return str(candidate)
    raise WorkflowError(
        f"required LLVM tool not found: {names[0]}. Install LLVM and add its bin directory to PATH"
    )


def discover_tools(
    mode: str,
    *,
    platform_name: str,
    environ: Mapping[str, str],
) -> Tools:
    clang_tidy = find_executable(
        ("clang-tidy",),
        platform_name=platform_name,
        environ=environ,
    )
    run_clang_tidy = find_executable(
        ("run-clang-tidy", "run-clang-tidy.py"),
        platform_name=platform_name,
        environ=environ,
    )
    apply_replacements = None
    if mode == "fix":
        apply_replacements = find_executable(
            ("clang-apply-replacements",),
            platform_name=platform_name,
            environ=environ,
        )
    return Tools(clang_tidy, run_clang_tidy, apply_replacements)


def _run(command_runner: CommandRunner, command: list[str], workspace: Path) -> int:
    try:
        result = command_runner(command, cwd=workspace, check=False)
    except OSError as error:
        raise WorkflowError(f"could not execute {command[0]}: {error}") from error
    return result.returncode


def _executable_command(executable: str) -> list[str]:
    if Path(executable).suffix.lower() == ".py":
        return [sys.executable, executable]
    return [executable]


def _malformed_fixes(path: Path, detail: str) -> WorkflowError:
    return WorkflowError(f"exported fixes {path} are malformed: {detail}")


def _replacement_messages(
    path: Path,
    diagnostic: dict[str, object],
) -> list[dict[str, object]]:
    primary = diagnostic.get("DiagnosticMessage")
    if not isinstance(primary, dict):
        raise _malformed_fixes(path, "diagnostic lacks DiagnosticMessage")
    messages = [primary]
    notes = diagnostic.get("Notes", [])
    if not isinstance(notes, list) or any(not isinstance(note, dict) for note in notes):
        raise _malformed_fixes(path, "diagnostic Notes is not a list of messages")
    messages.extend(notes)
    return messages


def _parse_replacement(path: Path, value: object) -> _Replacement:
    if not isinstance(value, dict):
        raise _malformed_fixes(path, "replacement is not an object")
    expected_types = {
        "FilePath": str,
        "Offset": int,
        "Length": int,
        "ReplacementText": str,
    }
    for field, expected_type in expected_types.items():
        field_value = value.get(field)
        if not isinstance(field_value, expected_type) or (
            expected_type is int and isinstance(field_value, bool)
        ):
            raise _malformed_fixes(
                path,
                f"replacement field {field} has the wrong type or is missing",
            )
    file_path = value["FilePath"]
    offset = value["Offset"]
    length = value["Length"]
    text = value["ReplacementText"]
    assert isinstance(file_path, str)
    assert isinstance(offset, int)
    assert isinstance(length, int)
    assert isinstance(text, str)
    if not file_path:
        raise _malformed_fixes(path, "replacement FilePath is empty")
    if offset < 0 or length < 0:
        raise _malformed_fixes(path, "replacement Offset and Length must be non-negative")
    return _Replacement(file_path, offset, length, text)


def _replacement_file_identity(
    file_path: str,
    base_directory: Path,
) -> tuple[object, ...]:
    target = Path(file_path)
    if not target.is_absolute():
        target = base_directory / target
    try:
        status = target.stat()
    except OSError:
        fallback = os.path.abspath(os.path.normpath(target))
        return ("path", os.path.normcase(fallback))
    return ("file", status.st_dev, status.st_ino)


def normalize_replacements(
    fixes_directory: Path,
    base_directory: Path | None = None,
) -> None:
    """Remove duplicate and conflicting replacements before applying fixes."""
    replacement_base = (base_directory or Path.cwd()).resolve()
    documents: list[tuple[Path, dict[str, object]]] = []
    occurrences: list[_Replacement] = []
    replacement_lists: list[tuple[list[object], list[int]]] = []
    groups: dict[tuple[tuple[object, ...], int, int], list[int]] = {}

    for path in sorted(fixes_directory.glob("*.yaml")):
        try:
            document = yaml.safe_load(path.read_text())
        except (OSError, UnicodeError, yaml.YAMLError) as error:
            raise _malformed_fixes(path, str(error)) from error
        if document is None:
            continue
        if not isinstance(document, dict):
            raise _malformed_fixes(path, "expected a YAML mapping")
        if not isinstance(document.get("MainSourceFile"), str):
            raise _malformed_fixes(path, "MainSourceFile is missing or is not a string")
        diagnostics = document.get("Diagnostics")
        if not isinstance(diagnostics, list):
            raise _malformed_fixes(path, "Diagnostics is missing or is not a list")
        documents.append((path, document))
        for diagnostic in diagnostics:
            if not isinstance(diagnostic, dict):
                raise _malformed_fixes(path, "diagnostic is not an object")
            for message in _replacement_messages(path, diagnostic):
                replacements = message.get("Replacements")
                if not isinstance(replacements, list):
                    raise _malformed_fixes(
                        path,
                        "diagnostic message Replacements is missing or is not a list",
                    )
                indexes: list[int] = []
                for value in replacements:
                    replacement = _parse_replacement(path, value)
                    index = len(occurrences)
                    occurrences.append(replacement)
                    indexes.append(index)
                    file_identity = _replacement_file_identity(
                        replacement.file_path,
                        replacement_base,
                    )
                    key = (file_identity, replacement.offset, replacement.length)
                    groups.setdefault(key, []).append(index)
                replacement_lists.append((replacements, indexes))

    retained: set[int] = set()
    for indexes in groups.values():
        texts = {occurrences[index].text for index in indexes}
        if len(texts) == 1:
            retained.add(indexes[0])
            continue
        replacement = occurrences[indexes[0]]
        print(
            "clang-tidy: skipped "
            f"{len(indexes)} conflicting replacements for {replacement.file_path} "
            f"at offset {replacement.offset}, length {replacement.length}."
        )

    # Same-range conflicts are handled above; replacements whose ranges *partially*
    # overlap within one file also make clang-apply-replacements abort (we no longer
    # pass -ignore-insert-conflict). Drop every replacement involved in an overlap so
    # none reach the tool. Ranges are half-open [offset, offset + length); a
    # zero-length insertion strictly inside another replacement's range counts as
    # overlapping, while insertions at a boundary and disjoint ranges are preserved.
    retained_ranges: dict[tuple[object, ...], list[tuple[int, int, int]]] = {}
    for (file_identity, offset, length), group_indexes in groups.items():
        representative = group_indexes[0]
        if representative in retained:
            retained_ranges.setdefault(file_identity, []).append((offset, length, representative))

    overlapping: set[int] = set()
    for ranges in retained_ranges.values():
        ordered = sorted(ranges)
        for position, (offset, length, index) in enumerate(ordered):
            end = offset + length
            for other_offset, other_length, other_index in ordered[position + 1 :]:
                if other_offset >= end:
                    break
                if offset < other_offset + other_length:
                    overlapping.add(index)
                    overlapping.add(other_index)

    if overlapping:
        retained -= overlapping
        for ranges in retained_ranges.values():
            dropped = [index for _, _, index in ranges if index in overlapping]
            if dropped:
                replacement = occurrences[dropped[0]]
                print(
                    "clang-tidy: skipped "
                    f"{len(dropped)} overlapping replacements for "
                    f"{replacement.file_path}."
                )

    for replacements, indexes in replacement_lists:
        replacements[:] = [
            replacement for replacement, index in zip(replacements, indexes) if index in retained
        ]

    for path, document in documents:
        try:
            path.write_text(
                yaml.safe_dump(
                    document,
                    explicit_end=True,
                    explicit_start=True,
                    sort_keys=False,
                )
            )
        except (OSError, UnicodeError, yaml.YAMLError) as error:
            raise WorkflowError(f"could not normalize exported fixes {path}: {error}") from error


def _macos_sdk_path(command_runner: CommandRunner, workspace: Path) -> str:
    command = ["xcrun", "--show-sdk-path"]
    try:
        result = command_runner(
            command,
            cwd=workspace,
            check=False,
            stdout=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise WorkflowError(
            f"could not execute xcrun --show-sdk-path: {error}; "
            "install the Xcode Command Line Tools"
        ) from error
    if result.returncode:
        raise WorkflowError(
            f"xcrun --show-sdk-path failed with exit code {result.returncode}; "
            "install the Xcode Command Line Tools"
        )
    sdk_path = (result.stdout or "").strip()
    if not sdk_path:
        raise WorkflowError(
            "xcrun --show-sdk-path returned an empty path; install the Xcode Command Line Tools"
        )
    return sdk_path


def run_workflow(
    *,
    mode: str,
    workspace: Path,
    compdb_tool: str,
    platform_name: str,
    environ: Mapping[str, str],
    command_runner: CommandRunner = subprocess.run,
) -> int:
    if mode not in ("report", "fix"):
        raise WorkflowError(f"unsupported mode: {mode}")
    if mode == "fix" and platform_name not in ("darwin", "linux"):
        raise WorkflowError("clang-tidy fix mode is supported only on macOS and Linux")

    refresh_code = _run(command_runner, [compdb_tool], workspace)
    if refresh_code:
        raise WorkflowError(f"compilation database refresher failed with exit code {refresh_code}")

    entries = load_project_entries(workspace, workspace / "compile_commands.json")
    tools = discover_tools(mode, platform_name=platform_name, environ=environ)
    macos_sdk = None
    if platform_name == "darwin":
        macos_sdk = _macos_sdk_path(command_runner, workspace)

    with tempfile.TemporaryDirectory(prefix="fastecu-clang-tidy-") as directory:
        filtered_database = Path(directory) / "compile_commands.json"
        filtered_database.write_text(json.dumps(entries, indent=2) + "\n")
        command = _executable_command(tools.run_clang_tidy) + [
            "-clang-tidy-binary",
            tools.clang_tidy,
            "-config-file",
            str(workspace / ".clang-tidy"),
            "-p",
            directory,
        ]
        if macos_sdk is not None:
            command.extend(
                [
                    "-extra-arg-before=-isysroot",
                    f"-extra-arg-before={macos_sdk}",
                ]
            )
        fixes_directory = Path(directory) / "fixes"
        if mode == "fix":
            fixes_directory.mkdir()
            command.extend(["-export-fixes", str(fixes_directory) + os.sep])
        print(f"Running clang-tidy in {mode} mode over {len(entries)} translation units.")
        tidy_code = _run(command_runner, command, workspace)
        if tidy_code:
            raise WorkflowError(f"run-clang-tidy failed with exit code {tidy_code}")
        if mode == "fix":
            assert tools.clang_apply_replacements is not None
            normalize_replacements(fixes_directory, workspace)
            apply_code = _run(
                command_runner,
                [
                    tools.clang_apply_replacements,
                    str(fixes_directory),
                ],
                workspace,
            )
            if apply_code:
                raise WorkflowError(f"replacement application failed with exit code {apply_code}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("report", "fix"))
    parser.add_argument("--compdb-tool", required=True)
    args = parser.parse_args(argv)
    try:
        root = workspace_root(os.environ)
        tool = Path(args.compdb_tool)
        if not tool.is_absolute():
            tool = (Path.cwd() / tool).resolve()
        return run_workflow(
            mode=args.mode,
            workspace=root,
            compdb_tool=str(tool),
            platform_name=sys.platform,
            environ=os.environ,
        )
    except WorkflowError as error:
        print(f"clang-tidy: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
