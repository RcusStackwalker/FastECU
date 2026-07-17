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

SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx"))


class WorkflowError(RuntimeError):
    """An actionable clang-tidy workflow failure."""


@dataclass(frozen=True)
class Tools:
    clang_tidy: str
    run_clang_tidy: str
    clang_apply_replacements: str | None = None


CommandRunner = Callable[..., subprocess.CompletedProcess[str]]


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
        if source.is_file() and source.suffix.lower() in SOURCE_SUFFIXES:
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
            "xcrun --show-sdk-path returned an empty path; "
            "install the Xcode Command Line Tools"
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
            apply_code = _run(
                command_runner,
                [
                    tools.clang_apply_replacements,
                    "-ignore-insert-conflict",
                    str(fixes_directory),
                ],
                workspace,
            )
            if apply_code:
                raise WorkflowError(
                    f"replacement application failed with exit code {apply_code}"
                )
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
