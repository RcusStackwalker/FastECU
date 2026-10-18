"""Turn `run-clang-tidy` output into readable findings and GitHub annotations."""

from __future__ import annotations

import re
import shlex
from dataclasses import dataclass
from pathlib import Path, PurePath, PurePosixPath, PureWindowsPath

# The path is matched lazily so a Windows drive letter (`C:\...`) stays part
# of it; the check suffix is optional because notes carry none.
_DIAGNOSTIC = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+): "
    r"(?P<severity>fatal error|error|warning|note|remark): "
    r"(?P<message>.*?)(?: \[(?P<check>[^\]]+)\])?$"
)
_WARNINGS_AS_ERRORS = ",-warnings-as-errors"
# run-clang-tidy echoes `[<i>/<N>][<secs>s] <shlex.join(invocation)>` as each
# translation unit finishes; the source file is the invocation's last argument.
_PROGRESS = re.compile(r"^\[\s*(?P<index>\d+)/(?P<total>\d+)\]\[[\d.]+s\] (?P<command>.+)$")
# clang's source snippet and caret lines: `   12 |   foo();`, `      |   ^~~`.
_DETAIL = re.compile(r"^\s*(\d+\s*)?\|")
_NOISE = tuple(
    re.compile(pattern)
    for pattern in (
        r"^\s*$",
        r"^\d+ (warnings?|errors?)( and \d+ (warnings?|errors?))? generated\.$",
        r"^Suppressed \d+ warnings? ",
        r"^Use -header-filter=",
        r"^\d+ warnings? treated as errors?$",
        r"^Running clang-tidy in \d+ threads ",
        # The -enable-check-profile report.
        r"^=+-+=+$",
        r"^\s*clang-tidy checks profiling$",
        r"^\s*Total Execution Time: ",
        r"^\s*---User Time---",
        r"^\s*\d+(\.\d+)?\s+\(\s*\d+(\.\d+)?%\)",
    )
)
_DRIVE = re.compile(r"^[A-Za-z]:[\\/]")


@dataclass(frozen=True)
class Diagnostic:
    path: str
    line: int
    column: int
    severity: str
    message: str
    check: str | None


def parse_diagnostic(line: str) -> Diagnostic | None:
    match = _DIAGNOSTIC.match(line)
    if match is None:
        return None
    check = match["check"]
    if check is not None:
        check = check.removesuffix(_WARNINGS_AS_ERRORS)
    return Diagnostic(
        match["path"],
        int(match["line"]),
        int(match["column"]),
        match["severity"],
        match["message"],
        check,
    )


def parse_progress(line: str) -> tuple[int, int, str] | None:
    """(index, total, source file) from run-clang-tidy's per-unit echo."""
    match = _PROGRESS.match(line)
    if match is None:
        return None
    command = match["command"]
    try:
        tokens = shlex.split(command)
    except ValueError:
        tokens = command.split()
    return int(match["index"]), int(match["total"]), tokens[-1]


def is_detail(line: str) -> bool:
    return _DETAIL.match(line) is not None


def is_noise(line: str) -> bool:
    return any(pattern.match(line) for pattern in _NOISE)


def _relative(path: str, workspace: PurePath) -> PurePath | None:
    flavour = PureWindowsPath if _DRIVE.match(path) else PurePosixPath
    try:
        return flavour(path).relative_to(workspace)
    except ValueError, TypeError:
        pass
    # The diagnostic may spell the workspace through a symlink.
    try:
        return Path(path).resolve().relative_to(Path(workspace).resolve())
    except ValueError, OSError:
        return None


def display_path(path: str, workspace: PurePath) -> str:
    relative = _relative(path, workspace)
    return path if relative is None else relative.as_posix()


def annotation_path(path: str, workspace: PurePath) -> str | None:
    """The repo-relative path GitHub can annotate, or None outside the repo."""
    relative = _relative(path, workspace)
    if relative is None or not relative.parts:
        return None
    first = relative.parts[0]
    if first == "external" or first.startswith("bazel-"):
        return None
    return relative.as_posix()
