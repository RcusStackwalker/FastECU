#!/usr/bin/env python3
"""Run the clang-tidy Bazel aspect, show its findings, and optionally apply its fixes.

The aspect (//tools/clang-tidy/aspect) runs clang-tidy once per source as a cacheable action.
This script builds with it, reads the outputs through a build event file, prints the findings the
way the compile-database runner does, and with --fix applies the exported replacements with
clang-apply-replacements and checks again.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path, PurePath
from urllib.parse import unquote, urlparse

import clang_tidy_output as output

ASPECT = "//tools/clang-tidy/aspect:linters.bzl%clang_tidy"
GROUPS = "rules_lint_human,rules_lint_fixes"
MAX_FIX_PASSES = 3
# With a remote cache Bazel may leave outputs there; the lint output has to be read from disk.
DOWNLOAD_ARGS = (
    r"--remote_download_regex=.*\.AspectRulesLintClangTidy\.(out|out\.exit_code|fixes\.yaml)$",
)
HOMEBREW_BIN = (Path("/opt/homebrew/opt/llvm/bin"), Path("/usr/local/opt/llvm/bin"))

# Bazel runs actions in <output base>/sandbox/<kind>/<n>/execroot/<workspace>/; clang-tidy and its
# exported fixes name files by that path.
_EXECROOT = re.compile(r"(?:[A-Za-z]:)?[^\s:'\"]*[\\/]execroot[\\/][^\\/\s'\"]+")

Run = Callable[..., "subprocess.CompletedProcess[str]"]


class AspectError(RuntimeError):
    """A failure of the tooling around the aspect, not a clang-tidy finding."""


@dataclass(frozen=True)
class OutputFiles:
    human: list[Path] = field(default_factory=list)
    exit_codes: list[Path] = field(default_factory=list)
    fixes: list[Path] = field(default_factory=list)
    sources: dict[Path, str] = field(default_factory=dict)


def localize(text: str, workspace: Path) -> str:
    """Rewrite sandbox execroot paths in `text` to the workspace."""
    return _EXECROOT.sub(workspace.as_posix(), text)


def _file_path(uri: str) -> Path:
    parsed = urlparse(uri)
    if parsed.scheme != "file":
        raise AspectError(
            f"an output was not downloaded from the remote cache ({uri}); "
            "pass --bazel-arg=--remote_download_toplevel or a --remote_download_regex"
        )
    path = unquote(parsed.path)
    if os.name == "nt" and re.match(r"^/[A-Za-z]:", path):
        path = path[1:]
    if parsed.netloc and parsed.netloc != "localhost":
        if os.name != "nt":
            raise AspectError(f"unsupported file URI host: {parsed.netloc}")
        path = f"//{parsed.netloc}{path}"
    return Path(path)


def _source_name(name: str) -> str | None:
    match = re.search(r"_rules_lint/(.+)\.AspectRulesLintClangTidy\.", name)
    return match[1] if match else None


def _read_output(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise AspectError(f"cannot read lint output {path}: {error}") from error


def _exit_status(path: Path) -> int:
    text = _read_output(path).strip()
    if not re.fullmatch(r"-?\d+", text):
        raise AspectError(f"invalid lint exit code in {path}: {text!r}")
    return int(text)


def output_files(events_path: Path) -> OutputFiles:
    """The aspect outputs named by a build event file, classified by suffix."""
    sets: dict[str, dict[str, object]] = {}
    groups: list[dict[str, object]] = []
    try:
        for line in _read_output(events_path).splitlines():
            event = json.loads(line)
            named = event.get("namedSetOfFiles")
            if named is not None:
                sets[event["id"]["namedSet"]["id"]] = named
            groups.extend(event.get("completed", {}).get("outputGroup", []))
        return _collect_outputs(sets, groups)
    except (ValueError, KeyError, TypeError) as error:
        raise AspectError(f"invalid build events: {error}") from error


def _collect_outputs(sets: Mapping[str, dict], groups: Sequence[dict]) -> OutputFiles:

    wanted = set(GROUPS.split(","))
    paths: dict[Path, None] = {}
    names: dict[Path, str] = {}
    pending = [
        ref["id"] for group in groups if group["name"] in wanted for ref in group["fileSets"]
    ]
    seen: set[str] = set()
    while pending:
        identifier = pending.pop()
        if identifier in seen:
            continue
        seen.add(identifier)
        if identifier not in sets:
            raise AspectError(f"missing build event file set: {identifier}")
        named = sets[identifier]
        for file in named.get("files", []):
            path = _file_path(file["uri"])
            paths[path] = None
            names[path] = "/".join([*file.get("pathPrefix", []), file["name"]])
        pending.extend(ref["id"] for ref in named.get("fileSets", []))

    ordered = sorted(paths)
    all_names = set(names.values())
    for name in names.values():
        if name.endswith(".AspectRulesLintClangTidy.out") and name + ".exit_code" not in all_names:
            raise AspectError(f"missing lint exit code for {name}")
    return OutputFiles(
        human=[p for p in ordered if names[p].endswith(".AspectRulesLintClangTidy.out")],
        exit_codes=[
            p for p in ordered if names[p].endswith(".AspectRulesLintClangTidy.out.exit_code")
        ],
        fixes=[p for p in ordered if names[p].endswith(".AspectRulesLintClangTidy.fixes.yaml")],
        sources={p: source for p in ordered if (source := _source_name(names[p])) is not None},
    )


def select_outputs(files: OutputFiles, prefixes: Sequence[PurePath]) -> OutputFiles:
    """Select source outputs by their BEP identity, independently of cache filenames."""
    if not prefixes:
        return files
    selected = {
        path: source
        for path, source in files.sources.items()
        if any(
            prefix == PurePath(source) or prefix in PurePath(source).parents for prefix in prefixes
        )
    }
    return OutputFiles(
        human=[p for p in files.human if p in selected],
        exit_codes=[p for p in files.exit_codes if p in selected],
        fixes=[p for p in files.fixes if p in selected],
        sources=selected,
    )


def source_count(files: OutputFiles) -> int:
    """How many sources were linted; a target with no sources has a placeholder output."""
    return sum(
        path in files.sources or any(part.endswith("_rules_lint") for part in path.parts)
        for path in files.human
    )


def report(
    files: OutputFiles,
    *,
    workspace: Path,
    write: Callable[[str], object],
    color: bool,
    annotate: bool,
) -> int:
    """Print the findings and return how many there were (failed runs count as one each)."""
    failed = [path for path in files.exit_codes if _exit_status(path) != 0]
    stream = output.OutputStream(
        workspace=workspace,
        write=lambda text: write(text + "\n"),
        color=color,
        annotate=annotate,
    )
    for path in files.human:
        for line in localize(_read_output(path), workspace).splitlines():
            stream.feed(line)
    findings = stream.finish().unique
    if findings:
        return findings
    # A run that failed without producing a parseable finding must not pass silently.
    for path in failed:
        write(f"clang-tidy failed without a diagnostic: {path.name}\n")
    return len(failed)


def apply_fixes(
    fixes: Sequence[Path], *, workspace: Path, tool: str, run: Run = subprocess.run
) -> int:
    """Apply the replacements in `fixes`; return how many reports had any."""
    with tempfile.TemporaryDirectory(prefix="fastecu-clang-tidy-fixes-") as name:
        directory = Path(name)
        count = 0
        for index, path in enumerate(fixes):
            text = path.read_text(encoding="utf-8")
            if "Replacements:" not in text:
                continue
            (directory / f"{index}.yaml").write_text(localize(text, workspace), encoding="utf-8")
            count += 1
        if not count:
            return 0
        result = run(
            [tool, str(directory)], cwd=workspace, check=False, text=True, capture_output=True
        )
        if result.returncode:
            raise AspectError(f"clang-apply-replacements failed: {result.stderr or result.stdout}")
        return count


def build_command(targets: Sequence[str], events: Path, extra: Sequence[str] = ()) -> list[str]:
    return [
        "bazel",
        "build",
        "--keep_going",
        f"--aspects={ASPECT}",
        f"--output_groups={GROUPS}",
        f"--build_event_json_file={events}",
        # With a remote cache the default converts file URIs to bytestream:// ones, which hide
        # that the output is on disk.
        "--nobuild_event_json_file_path_conversion",
        *extra,
        *targets,
    ]


def find_apply_replacements(
    environ: Mapping[str, str],
    *,
    which: Callable[[str], str | None] = shutil.which,
    homebrew: Sequence[Path] = HOMEBREW_BIN,
) -> str:
    name = "clang-apply-replacements"
    roots = [Path(environ["FASTECU_LLVM_ROOT"]) / "bin"] if "FASTECU_LLVM_ROOT" in environ else []
    for directory in roots:
        candidate = directory / name
        if candidate.is_file():
            return str(candidate)
    found = which(name)
    if found:
        return found
    for directory in homebrew:
        if (directory / name).is_file():
            return str(directory / name)
    raise AspectError(f"{name} not found; install LLVM or set FASTECU_LLVM_ROOT")


def _build_and_report(
    args: argparse.Namespace, workspace: Path, run: Run
) -> tuple[int, OutputFiles]:
    with tempfile.TemporaryDirectory(prefix="fastecu-clang-tidy-") as name:
        events = Path(name) / "events.json"
        extra = [*DOWNLOAD_ARGS, *args.bazel_arg]
        build = run(build_command(args.targets, events, extra), cwd=workspace, check=False)
        if not events.is_file():
            raise AspectError(f"bazel build produced no build events (exit {build.returncode})")
        files = output_files(events)
        if not source_count(files):
            raise AspectError(
                "the build produced no lint output for any source "
                f"({len(files.human)} outputs); a run that checks nothing must not pass"
            )
        # The outputs live under bazel-out, which outlasts the temporary event file.
        findings = report(
            files,
            workspace=workspace,
            write=sys.stdout.write,
            color=sys.stdout.isatty(),
            annotate=args.annotate,
        )
    if build.returncode:
        raise AspectError(f"bazel build failed with exit code {build.returncode}")
    return findings, files


def main(argv: Sequence[str] | None = None, run: Run = subprocess.run) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("targets", nargs="*", default=["//..."])
    parser.add_argument("--fix", action="store_true", help="apply suggested fixes, then re-check")
    parser.add_argument("--annotate", action="store_true", help="also print GitHub annotations")
    parser.add_argument("--bazel-arg", action="append", default=[], help="extra `bazel build` flag")
    args = parser.parse_args(argv)
    workspace = Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY", Path.cwd()))
    try:
        for attempt in range(MAX_FIX_PASSES if args.fix else 1):
            findings, files = _build_and_report(args, workspace, run)
            if not args.fix or not findings:
                break
            tool = find_apply_replacements(os.environ)
            if not apply_fixes(files.fixes, workspace=workspace, tool=tool, run=run):
                break
            print(f"clang-tidy: applied fixes (pass {attempt + 1}); checking again.")
        noun = "finding" if findings == 1 else "findings"
        print(f"clang-tidy: {source_count(files)} files, {findings} {noun}")
        return 1 if findings else 0
    except AspectError as error:
        print(f"clang-tidy: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
