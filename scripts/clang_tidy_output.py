"""Turn `run-clang-tidy` output into readable findings and GitHub annotations."""

from __future__ import annotations

import re
import shlex
from collections.abc import Callable
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
_RESET = "\x1b[0m"
_DIM = "\x1b[2m"
_SEVERITY_COLOURS = {
    "fatal error": "\x1b[1;31m",
    "error": "\x1b[1;31m",
    "warning": "\x1b[1;33m",
    "note": "\x1b[1;36m",
    "remark": "\x1b[1;36m",
}
_ANNOTATION_LEVELS = {
    "fatal error": "error",
    "error": "error",
    "warning": "warning",
    "remark": "notice",
}


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


@dataclass(frozen=True)
class Finding:
    diagnostic: Diagnostic
    detail: tuple[str, ...] = ()
    notes: tuple[Finding, ...] = ()


def _severity(severity: str, color: bool) -> str:
    return f"{_SEVERITY_COLOURS[severity]}{severity}{_RESET}" if color else severity


def _location(diagnostic: Diagnostic, workspace: PurePath) -> str:
    path = display_path(diagnostic.path, workspace)
    return f"{path}:{diagnostic.line}:{diagnostic.column}"


def format_finding(finding: Finding, *, workspace: PurePath, color: bool) -> str:
    """A finding as a console block: header, message, snippet, then notes."""
    diagnostic = finding.diagnostic
    header = f"{_severity(diagnostic.severity, color)}: {_location(diagnostic, workspace)}"
    if diagnostic.check is not None:
        check = f"[{diagnostic.check}]"
        header += f" {_DIM}{check}{_RESET}" if color else f" {check}"
    lines = [header, f"  {diagnostic.message}"]
    lines.extend(f"    {detail}" for detail in finding.detail)
    for note in finding.notes:
        lines.append(
            f"  {_severity(note.diagnostic.severity, color)}: "
            f"{_location(note.diagnostic, workspace)} {note.diagnostic.message}"
        )
        lines.extend(f"    {detail}" for detail in note.detail)
    return "\n".join(lines)


def _escape_data(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def _escape_property(text: str) -> str:
    return _escape_data(text).replace(":", "%3A").replace(",", "%2C")


def format_annotation(finding: Finding, *, workspace: PurePath) -> str | None:
    """A GitHub workflow-command annotation, or None for a file outside the repo."""
    diagnostic = finding.diagnostic
    path = annotation_path(diagnostic.path, workspace)
    if path is None:
        return None
    level = _ANNOTATION_LEVELS.get(diagnostic.severity, "notice")
    title = diagnostic.check or diagnostic.severity
    body = [diagnostic.message]
    body.extend(
        f"note: {_location(note.diagnostic, workspace)} {note.diagnostic.message}"
        for note in finding.notes
    )
    message = "\n".join(body)
    return (
        f"::{level} file={_escape_property(path)},line={diagnostic.line},"
        f"col={diagnostic.column},title={_escape_property(title)}::"
        f"{_escape_data(message)}"
    )


@dataclass(frozen=True)
class StreamCounts:
    unique: int
    raw: int


class OutputStream:
    """Formats run-clang-tidy output line by line as it arrives.

    A finding is written once the line after it shows it is complete, so its
    snippet and notes stay together; repeats of the same finding (a header
    seen from several translation units) are written once.
    """

    def __init__(
        self,
        *,
        workspace: PurePath,
        write: Callable[[str], None],
        color: bool,
        annotate: bool,
    ) -> None:
        self._workspace = workspace
        self._write = write
        self._color = color
        self._annotate = annotate
        self._seen: set[tuple[str, int, int, str]] = set()
        self._raw = 0
        self._diagnostic: Diagnostic | None = None
        self._detail: list[str] = []
        self._notes: list[tuple[Diagnostic, list[str]]] = []

    def feed(self, line: str) -> None:
        diagnostic = parse_diagnostic(line)
        if diagnostic is not None and diagnostic.severity == "note" and self._diagnostic:
            self._notes.append((diagnostic, []))
            return
        if diagnostic is not None and diagnostic.severity != "note":
            self._flush()
            self._diagnostic = diagnostic
            return
        if self._diagnostic is not None and is_detail(line):
            (self._notes[-1][1] if self._notes else self._detail).append(line)
            return
        self._flush()
        progress = parse_progress(line)
        if progress is not None:
            index, total, source = progress
            self._write(f"[{index}/{total}] {display_path(source, self._workspace)}")
        elif not is_noise(line):
            self._write(line)

    def finish(self) -> StreamCounts:
        self._flush()
        return StreamCounts(unique=len(self._seen), raw=self._raw)

    def _flush(self) -> None:
        diagnostic = self._diagnostic
        if diagnostic is None:
            return
        notes = tuple(Finding(note, tuple(detail)) for note, detail in self._notes)
        finding = Finding(diagnostic, tuple(self._detail), notes)
        self._diagnostic = None
        self._detail = []
        self._notes = []
        self._raw += 1
        key = (diagnostic.path, diagnostic.line, diagnostic.column, diagnostic.check or "")
        if key in self._seen:
            return
        self._seen.add(key)
        self._write(format_finding(finding, workspace=self._workspace, color=self._color))
        if self._annotate:
            annotation = format_annotation(finding, workspace=self._workspace)
            if annotation is not None:
                self._write(annotation)
