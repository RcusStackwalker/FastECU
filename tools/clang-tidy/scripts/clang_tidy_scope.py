"""Per-OS scope manifest for the clang-tidy runner."""

from __future__ import annotations

import os
import tomllib
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from itertools import chain
from pathlib import Path, PurePath, PurePosixPath, PureWindowsPath

MANIFEST_NAME = ".clang-tidy-scope.toml"


class ScopeError(ValueError):
    """The scope manifest is missing, malformed, or names an unsafe path."""


@dataclass(frozen=True)
class ScopeManifest:
    """Which code each OS's tidy run owns.

    `os_prefixes` maps an OS to the directories only it builds; `linux_covered`
    lists platform-gated directories the full Linux run already analyzes.
    """

    os_prefixes: Mapping[str, tuple[PurePath, ...]]
    linux_covered: tuple[PurePath, ...]


def _prefixes(value: object, where: str) -> tuple[PurePath, ...]:
    if not isinstance(value, list):
        raise ScopeError(f"{where}: prefixes must be a list")
    result: list[PurePath] = []
    for item in value:
        if not isinstance(item, str) or not item:
            raise ScopeError(f"{where}: prefix {item!r} is not a non-empty string")
        posix = PurePosixPath(item)
        windows = PureWindowsPath(item)
        if (
            posix.is_absolute()
            or windows.is_absolute()
            or windows.drive
            or ".." in posix.parts
            or ".." in windows.parts
        ):
            raise ScopeError(f"{where}: prefix {item!r} must be relative and stay in the workspace")
        result.append(PurePath(*posix.parts))
    return tuple(result)


def load_manifest(path: Path) -> ScopeManifest:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as error:
        raise ScopeError(f"cannot read scope manifest {path}: {error}") from error
    operating_systems = data.get("os", {})
    if not isinstance(operating_systems, dict):
        raise ScopeError("[os] must be a table")
    os_prefixes = {
        name: _prefixes(table.get("prefixes", []), f"[os.{name}]")
        for name, table in operating_systems.items()
        if isinstance(table, dict)
    }
    covered = data.get("linux_covered", {})
    if not isinstance(covered, dict):
        raise ScopeError("[linux_covered] must be a table")
    return ScopeManifest(
        os_prefixes=os_prefixes,
        linux_covered=_prefixes(covered.get("prefixes", []), "[linux_covered]"),
    )


def in_scope(relative: PurePath, prefixes: Iterable[PurePath]) -> bool:
    """True when `relative` is one of `prefixes` or lives beneath one."""
    return any(prefix == relative or prefix in relative.parents for prefix in prefixes)


def build_targets(prefixes: Iterable[PurePath]) -> list[str]:
    return [f"//{prefix.as_posix()}/..." for prefix in prefixes]


def uncovered_gated_packages(
    packages: Iterable[PurePath], manifest: ScopeManifest
) -> list[PurePath]:
    covered = [*chain.from_iterable(manifest.os_prefixes.values()), *manifest.linux_covered]
    return sorted(package for package in packages if not in_scope(package, covered))


def gated_packages(workspace: Path) -> list[PurePath]:
    """Find source-owning platform-gated packages without walking other checkouts."""
    packages = []
    for parent, directories, files in os.walk(workspace, followlinks=False):
        directory = Path(parent)
        directories[:] = [
            name
            for name in directories
            if not name.startswith(".")
            and not (directory / name).is_symlink()
            and not os.path.isjunction(directory / name)
        ]
        if not any(Path(name).suffix.lower() in (".c", ".cc", ".cpp", ".cxx") for name in files):
            continue
        for name in ("BUILD", "BUILD.bazel"):
            if name not in files:
                continue
            try:
                text = (directory / name).read_text(encoding="utf-8")
            except (OSError, UnicodeError) as error:
                raise ScopeError(f"cannot inspect {directory / name}: {error}") from error
            if "target_compatible_with" in text or "@platforms//os:" in text:
                packages.append(PurePath(directory.relative_to(workspace)))
    return sorted(set(packages))


def scope_prefixes(workspace: Path, scope_os: str | None) -> tuple[PurePath, ...]:
    """Validate platform coverage and return the explicitly selected source prefixes."""
    path = workspace / MANIFEST_NAME
    if scope_os is None and not path.exists():
        return ()
    manifest = load_manifest(path)
    if uncovered := uncovered_gated_packages(gated_packages(workspace), manifest):
        raise ScopeError(
            f"platform-gated packages are not named in {MANIFEST_NAME}: "
            + ", ".join(package.as_posix() for package in uncovered)
        )
    if scope_os is None:
        return ()
    if not (prefixes := manifest.os_prefixes.get(scope_os)):
        raise ScopeError(f"scope manifest has no nonempty section for {scope_os!r}")
    for prefix in prefixes:
        if not (workspace / prefix).is_dir():
            raise ScopeError(f"scope prefix {prefix.as_posix()} does not exist in the workspace")
    return prefixes
