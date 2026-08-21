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
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

import yaml

SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx"))
HEADER_SUFFIXES = frozenset((".h", ".hpp"))


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


def _entry_source_path(entry: dict[str, object], root: Path) -> Path:
    """Resolve a compile-database entry's absolute source path.

    Callers must already have validated that `directory` and `file` are
    present strings (load_project_entries does this before an entry survives
    into its returned list).

    Raises WorkflowError if the resolved path escapes `root`: the compile
    database is Bazel-generated, but its `directory`/`file` fields are still
    external data and must not be trusted to stay within the workspace.
    """
    directory = Path(str(entry["directory"]))
    if not directory.is_absolute():
        directory = root / directory
    source = Path(str(entry["file"]))
    if not source.is_absolute():
        source = directory / source
    resolved = source.resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as error:
        raise WorkflowError(
            f"compilation database entry resolves outside the workspace: {resolved}"
        ) from error
    return resolved


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

        try:
            source = _entry_source_path(entry, root)
        except WorkflowError:
            continue
        if source.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if not source.is_file():
            raise WorkflowError(f"missing workspace translation unit: {source}")
        entries.append(entry)

    if not entries:
        raise WorkflowError("compilation database contains no workspace translation units")
    return entries


def filter_changed_entries(
    entries: list[dict[str, object]],
    changed: Sequence[Path],
    root: Path,
) -> tuple[list[dict[str, object]], list[str]]:
    """Narrow `entries` to translation units affected by `changed`.

    Source files match by path directly. A changed header with no changed
    source file falls back to co-located sources in the same directory
    (foo.h -> foo.cpp, foo_test.cpp), matching this repo's package-owned test
    convention. A header with no co-located source in `entries` produces a
    note instead of a match; this design does not trace transitive includers.
    """
    by_path: dict[Path, dict[str, object]] = {}
    by_directory: dict[Path, list[tuple[str, dict[str, object]]]] = {}
    for entry in entries:
        source = _entry_source_path(entry, root)
        by_path[source] = entry
        by_directory.setdefault(source.parent, []).append((source.stem, entry))

    matched: dict[Path, dict[str, object]] = {}
    notes: list[str] = []
    for path in changed:
        resolved = path.resolve()
        suffix = resolved.suffix.lower()
        if suffix in SOURCE_SUFFIXES:
            entry = by_path.get(resolved)
            if entry is not None:
                matched[resolved] = entry
        elif suffix in HEADER_SUFFIXES:
            stem = resolved.stem
            candidates = by_directory.get(resolved.parent, [])
            found = [
                entry
                for candidate_stem, entry in candidates
                if candidate_stem in (stem, f"{stem}_test")
            ]
            if found:
                for entry in found:
                    matched[_entry_source_path(entry, root)] = entry
            else:
                notes.append(
                    f"clang-tidy: {resolved} changed with no co-located source in the "
                    "compile database; its includers were not checked."
                )

    return [matched[key] for key in sorted(matched)], notes


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


def _git_lines(command_runner: CommandRunner, args: Sequence[str], workspace: Path) -> list[str]:
    command = ["git", *args]
    try:
        result = command_runner(
            command,
            cwd=workspace,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise WorkflowError(f"could not execute {command[0]}: {error}") from error
    if result.returncode:
        raise WorkflowError(
            f"git {' '.join(args)} failed with exit code {result.returncode}: "
            f"{(result.stderr or '').strip()}"
        )
    return [line for line in (result.stdout or "").splitlines() if line]


def changed_files(workspace: Path, command_runner: CommandRunner) -> list[Path]:
    """Files changed relative to origin/master: committed, staged, unstaged, and new.

    Deleted files (present in a diff but no longer on disk) are dropped --
    there's nothing left for clang-tidy to analyze.
    """
    merge_base = _git_lines(command_runner, ["merge-base", "HEAD", "origin/master"], workspace)
    if not merge_base:
        raise WorkflowError("git merge-base HEAD origin/master produced no output")
    base = merge_base[0]

    relative_paths: set[str] = set()
    relative_paths.update(
        _git_lines(command_runner, ["diff", "--name-only", f"{base}..HEAD"], workspace)
    )
    relative_paths.update(_git_lines(command_runner, ["diff", "--name-only"], workspace))
    relative_paths.update(
        _git_lines(command_runner, ["diff", "--name-only", "--cached"], workspace)
    )
    relative_paths.update(
        _git_lines(command_runner, ["ls-files", "--others", "--exclude-standard"], workspace)
    )

    candidates = (workspace / path for path in relative_paths)
    return sorted({path.resolve() for path in candidates if path.is_file()})


def _run(command_runner: CommandRunner, command: list[str], workspace: Path) -> int:
    try:
        result = command_runner(command, cwd=workspace, check=False)
    except OSError as error:
        raise WorkflowError(f"could not execute {command[0]}: {error}") from error
    return result.returncode


def _run_quiet(
    command_runner: CommandRunner,
    command: list[str],
    workspace: Path,
) -> subprocess.CompletedProcess[str]:
    """Run a command, capturing combined output instead of streaming it live."""
    try:
        return command_runner(
            command,
            cwd=workspace,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except OSError as error:
        raise WorkflowError(f"could not execute {command[0]}: {error}") from error


_WINDOWS_EXECUTABLE_SUFFIXES = frozenset((".exe", ".bat", ".cmd", ".com"))


def _executable_command(executable: str, *, platform_name: str) -> list[str]:
    suffix = Path(executable).suffix.lower()
    if suffix == ".py":
        return [sys.executable, executable]
    # LLVM ships run-clang-tidy as a shebang'd Python script with no extension.
    # POSIX execs it fine via the shebang; Windows cannot, so route it through
    # the interpreter whenever it lacks a recognized native executable suffix.
    if platform_name.startswith("win") and suffix not in _WINDOWS_EXECUTABLE_SUFFIXES:
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


def _count_diagnostics(fixes_directory: Path) -> int:
    """Count exported diagnostics across a fixes directory.

    Lenient by design: report mode discards these files after counting them,
    so a malformed document is skipped rather than raising. Strict validation
    happens in normalize_replacements, which fix mode always runs before
    applying anything.
    """
    total = 0
    for path in sorted(fixes_directory.glob("*.yaml")):
        try:
            document = yaml.safe_load(path.read_text())
        except OSError, UnicodeError, yaml.YAMLError:
            continue
        if not isinstance(document, dict):
            continue
        diagnostics = document.get("Diagnostics")
        if isinstance(diagnostics, list):
            total += len(diagnostics)
    return total


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
            replacement
            for replacement, index in zip(replacements, indexes, strict=True)
            if index in retained
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


def _prebuild(command_runner: CommandRunner, build_args: Sequence[str], workspace: Path) -> None:
    """Best-effort build of the analyzed targets.

    Hedron's aquery-based compdb refresh never executes compile actions, so
    build-generated header trees (e.g. rules_qt's per-framework
    _virtual_includes symlink farms) may not exist on disk yet even though
    the emitted compile flags reference them. Building first materializes
    them; --keep_going mirrors Hedron's own guidance so an unrelated failure
    doesn't block analysis of everything else.

    Whatever compilation_mode this build uses must match --compdb-arg's (both
    resolve into the same bazel-out/<config> directory), or compile_commands.json
    ends up pointing at generated headers this step never wrote.
    """
    if not build_args:
        return
    print("Building analyzed targets so generated headers exist before analysis.")
    result = _run_quiet(command_runner, ["bazel", "build", "--keep_going", *build_args], workspace)
    if result.returncode:
        if result.stdout:
            print(result.stdout, end="")
        print(
            f"clang-tidy: warning: prebuild exited with code {result.returncode}; "
            "some generated headers may be stale or missing.",
            file=sys.stderr,
        )


def _post_fix_build(
    command_runner: CommandRunner,
    build_args: Sequence[str],
    workspace: Path,
) -> None:
    """Require the analyzed Bazel targets to compile after applying fixes."""
    if not build_args:
        return
    print("Building analyzed targets after applying clang-tidy fixes.")
    result = _run_quiet(command_runner, ["bazel", "build", *build_args], workspace)
    if result.returncode:
        if result.stdout:
            print(result.stdout, end="")
        raise WorkflowError(f"post-fix Bazel build failed with exit code {result.returncode}")


def run_workflow(
    *,
    mode: str,
    workspace: Path,
    compdb_tool: str,
    platform_name: str,
    environ: Mapping[str, str],
    command_runner: CommandRunner = subprocess.run,
    build_args: Sequence[str] = (),
    compdb_args: Sequence[str] = (),
    changed: bool = False,
) -> None:
    if mode not in ("report", "fix"):
        raise WorkflowError(f"unsupported mode: {mode}")
    if mode == "fix" and platform_name not in ("darwin", "linux"):
        raise WorkflowError("clang-tidy fix mode is supported only on macOS and Linux")

    _prebuild(command_runner, build_args, workspace)
    refresh_code = _run(command_runner, [compdb_tool, *compdb_args], workspace)
    if refresh_code:
        raise WorkflowError(f"compilation database refresher failed with exit code {refresh_code}")

    entries = load_project_entries(workspace, workspace / "compile_commands.json")
    if changed:
        changed_paths = changed_files(workspace, command_runner)
        entries, notes = filter_changed_entries(entries, changed_paths, workspace.resolve())
        for note in notes:
            print(note)
        if not entries:
            print("clang-tidy: no changed C/C++ translation units to analyze, skipping.")
            return
    tools = discover_tools(mode, platform_name=platform_name, environ=environ)
    macos_sdk = None
    if platform_name == "darwin":
        macos_sdk = _macos_sdk_path(command_runner, workspace)

    with tempfile.TemporaryDirectory(prefix="fastecu-clang-tidy-") as directory:
        temp_root = Path(directory).resolve()
        filtered_database = (temp_root / "compile_commands.json").resolve()
        if not filtered_database.is_relative_to(temp_root):
            raise WorkflowError(
                "internal error: filtered compilation database escaped its temp directory"
            )
        filtered_database.write_text(json.dumps(entries, indent=2) + "\n")
        command = _executable_command(tools.run_clang_tidy, platform_name=platform_name) + [
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
        fixes_directory.mkdir()
        command.extend(["-export-fixes", str(fixes_directory) + os.sep])
        print(f"Analyzing {len(entries)} translation units in {mode} mode.")
        tidy_result = _run_quiet(command_runner, command, workspace)
        tidy_code = tidy_result.returncode
        finding_count = _count_diagnostics(fixes_directory)
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
                if tidy_result.stdout:
                    print(tidy_result.stdout, end="")
                raise WorkflowError(f"replacement application failed with exit code {apply_code}")
            _post_fix_build(command_runner, build_args, workspace)
        if tidy_code:
            if tidy_result.stdout:
                print(tidy_result.stdout, end="")
            detail = f"{finding_count} findings; run-clang-tidy failed with exit code {tidy_code}"
            if mode == "fix":
                detail += "; exported fixes were applied before reporting the failure"
            raise WorkflowError(detail)
        print(f"clang-tidy: {len(entries)} files clean, 0 findings")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("report", "fix"))
    parser.add_argument("--changed", action="store_true")
    parser.add_argument("--compdb-tool", required=True)
    parser.add_argument("--build-arg", action="append", default=[], dest="build_args")
    parser.add_argument("--compdb-arg", action="append", default=[], dest="compdb_args")
    args = parser.parse_args(argv)
    try:
        root = workspace_root(os.environ)
        tool = Path(args.compdb_tool)
        if not tool.is_absolute():
            tool = (Path.cwd() / tool).resolve()
        run_workflow(
            mode=args.mode,
            workspace=root,
            compdb_tool=str(tool),
            platform_name=sys.platform,
            environ=os.environ,
            build_args=args.build_args,
            compdb_args=args.compdb_args,
            changed=args.changed,
        )
        return 0
    except WorkflowError as error:
        print(f"clang-tidy: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
