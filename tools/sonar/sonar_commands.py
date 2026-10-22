"""Validate actual configured compiler commands without rewriting their flags."""

import json
from collections.abc import Sequence
from pathlib import Path

EXTRACTION_FLAGS = (
    "--config=coverage",
    "--features=-compiler_param_file",
    "--features=-layering_check",
    "--host_features=-compiler_param_file",
    "--host_features=-layering_check",
)


class CommandError(ValueError):
    """Incomplete or unsupported Sonar compiler input."""


def _strings(value: object, field: str, *, nonempty: bool = True) -> list[str]:
    if not isinstance(value, list) or (nonempty and not value):
        raise CommandError(f"invalid {field}: expected a nonempty string list")
    if any(not isinstance(item, str) or not item for item in value):
        raise CommandError(f"invalid {field}: empty or non-string element")
    return value


def _command_record(command, fragment, path):
    if (
        not isinstance(command, dict)
        or not isinstance(command.get("file"), str)
        or not command["file"]
    ):
        raise CommandError(f"invalid source file: {path}")
    argv = _strings(command.get("arguments"), "arguments")
    if any(argument.startswith("@") for argument in argv):
        raise CommandError(f"unexpanded compiler response file: {path}")
    _strings(command.get("inputs"), "inputs")
    _strings(command.get("outputs"), "outputs")
    return command | {"owner": fragment["owner"], "configuration": fragment["configuration"]}


def _fragment_records(fragment, path):
    if not isinstance(fragment, dict) or type(fragment.get("version")) is not int:
        raise CommandError(f"invalid fragment version: {path}")
    if fragment["version"] != 1:
        raise CommandError(f"unsupported fragment version: {path}")
    for field in ("owner", "configuration"):
        if not isinstance(fragment.get(field), str) or not fragment[field]:
            raise CommandError(f"invalid {field}: {path}")
    values = fragment.get("commands")
    if not isinstance(values, list) or not values:
        raise CommandError(f"empty or invalid commands: {path}")
    return [_command_record(command, fragment, path) for command in values]


def load_fragments(paths: Sequence[Path]) -> list[dict]:
    """Read versioned owner/configuration fragments; retain each compile action."""
    records = []
    for path in paths:
        try:
            records.extend(_fragment_records(json.loads(path.read_text()), path))
        except (OSError, json.JSONDecodeError) as error:
            raise CommandError(f"cannot read command fragment {path}: {error}") from error
    if not records:
        raise CommandError("empty configured C++ scope")
    return records


def command_identity(record: dict) -> tuple:
    return record["owner"], record["configuration"], record["file"], tuple(record["outputs"])


def database_entries(records: Sequence[dict], execroot: Path) -> list[dict]:
    """Merge identical actions, preserving distinct owners and configurations."""
    unique = {}
    for record in records:
        identity = command_identity(record)
        if identity in unique and unique[identity] != record:
            raise CommandError(f"conflicting configured compiler commands: {identity}")
        unique[identity] = record
    entries = []
    for identity in sorted(unique):
        record = unique[identity]
        argv = record["arguments"]
        entry = {"directory": str(execroot), "file": record["file"], "arguments": argv}
        if "-o" in argv:
            position = argv.index("-o")
            if position + 1 >= len(argv):
                raise CommandError(f"missing compiler output argument: {record['file']}")
            entry["output"] = argv[position + 1]
        entries.append(entry)
    if not entries:
        raise CommandError("empty configured C++ scope")
    return entries


def validate_inputs(records: Sequence[dict], execroot: Path) -> None:
    """Reject absent inputs and broken generated/virtual-include symlinks."""
    inputs = {record["file"] for record in records}
    inputs.update(path for record in records for path in record["inputs"])
    missing = sorted(path for path in inputs if not (execroot / path).exists())
    if missing:
        raise CommandError(f"missing compiler inputs ({len(missing)}): {', '.join(missing[:10])}")
