"""Validate exact, justified exceptions and audit their use in full-scope runs."""

import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath, PureWindowsPath

import yaml


class ExceptionConfigError(RuntimeError):
    """Exception policy is malformed or no longer exercised."""


@dataclass(frozen=True)
class ExceptionRule:
    check: str
    file: str
    symbol: str
    rationale: str

    @property
    def identity(self) -> tuple[str, str, str]:
        return self.check, self.file, self.symbol


class _UniqueKeyLoader(yaml.SafeLoader):
    def construct_mapping(self, node, deep=False):
        keys = [self.construct_object(key, deep=deep) for key, _ in node.value]
        if any(keys.count(key) > 1 for key in keys):
            raise ExceptionConfigError("duplicate YAML mapping key")
        return super().construct_mapping(node, deep=deep)


def load_exceptions(path: Path) -> tuple[ExceptionRule, ...]:
    try:
        content = yaml.load(path.read_text(), Loader=_UniqueKeyLoader)
    except (OSError, yaml.YAMLError) as error:
        raise ExceptionConfigError(f"cannot read exception policy {path}: {error}") from error
    if not isinstance(content, dict) or set(content) != {"version", "exceptions"}:
        raise ExceptionConfigError("exception policy requires exactly version and exceptions")
    if type(content["version"]) is not int or content["version"] != 1:
        raise ExceptionConfigError("unsupported exception policy version; expected 1")
    if not isinstance(content["exceptions"], list):
        raise ExceptionConfigError("exceptions must be a list")
    rules = []
    identities = set()
    for entry in content["exceptions"]:
        if not isinstance(entry, dict) or set(entry) != {"check", "file", "symbol", "rationale"}:
            raise ExceptionConfigError("exception entry requires check, file, symbol and rationale")
        for key, value in entry.items():
            if not isinstance(value, str) or not value.strip():
                raise ExceptionConfigError(f"exception {key} must be a nonempty string")
        if not re.fullmatch(r"fastecu-[a-z0-9-]+", entry["check"]):
            raise ExceptionConfigError("exception check must name a repository check exactly")
        file = entry["file"]
        relative = PurePosixPath(file)
        if (
            relative.is_absolute()
            or PureWindowsPath(file).drive
            or "\\" in file
            or ".." in relative.parts
            or relative.as_posix() != file
            or file == "."
        ):
            raise ExceptionConfigError(
                f"exception path must be canonical and repository-relative: {file}"
            )
        if "\n" in entry["symbol"] or "\r" in entry["symbol"]:
            raise ExceptionConfigError("exception symbol must be a single exact identity")
        rule = ExceptionRule(**entry)
        if rule.identity in identities:
            raise ExceptionConfigError(f"duplicate exception: {rule.identity}")
        rules.append(rule)
        identities.add(rule.identity)
    return tuple(rules)


def validate_usage(
    rules: tuple[ExceptionRule, ...], used: set[tuple[str, str, str]], *, full_scope: bool
) -> None:
    configured = {rule.identity for rule in rules}
    if unknown := used - configured:
        raise ExceptionConfigError(f"unknown exception usage: {sorted(unknown)}")
    if full_scope and (unused := configured - used):
        raise ExceptionConfigError(f"unused exceptions: {sorted(unused)}")
