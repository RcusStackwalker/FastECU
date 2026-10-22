"""Transfer completed LLVM coverage with exact provenance and portable paths."""

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Mapping
from pathlib import Path, PurePosixPath

FILES = ("llvm-cov.report", "llvm-cov.headers")
SECTION = re.compile(r"^(/[^\n]*\.(?:c|cc|cpp|cxx|h|hh|hpp)):$", re.MULTILINE)


class ArtifactError(ValueError):
    """Coverage cannot be attributed or imported faithfully."""


def _digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def _read_report(directory: Path) -> tuple[str, list[str]]:
    report = (directory / FILES[0]).read_text()
    headers = (directory / FILES[1]).read_text().splitlines()
    if not headers or SECTION.findall(report) != headers:
        raise ArtifactError("coverage header manifest does not match the report")
    return report, headers


def export_report(
    coverage_dir: Path,
    destination: Path,
    *,
    commit: str,
    run_id: str,
    attempt: str,
    producer_root: Path,
    toolchain: Mapping[str, str],
    configuration: str = "coverage",
) -> Path:
    """Publish only the completed report, headers and versioned provenance."""
    try:
        if coverage_dir.resolve() == destination.resolve():
            raise ArtifactError("coverage export destination must differ from its source")
        _read_report(coverage_dir)
        if not commit or not run_id or not attempt or not toolchain:
            raise ArtifactError("coverage provenance is incomplete")
        destination.mkdir(parents=True, exist_ok=True)
        manifest = destination / "manifest.json"
        manifest.unlink(missing_ok=True)
        for name in FILES:
            shutil.copyfile(coverage_dir / name, destination / name)
        value = {
            "version": 1,
            "commit": commit,
            "workflow_run_id": run_id,
            "producer_attempt": attempt,
            "producer_root": str(producer_root.resolve()),
            "configuration": configuration,
            "toolchain": dict(toolchain),
            "files": {name: _digest(destination / name) for name in FILES},
        }
        manifest.write_text(json.dumps(value, indent=2) + "\n")
        return manifest
    except OSError as error:
        raise ArtifactError(f"cannot export coverage: {error}") from error


def _manifest(artifact_dir: Path, expected: dict) -> dict:
    value = json.loads((artifact_dir / "manifest.json").read_text())
    if (
        not isinstance(value, dict)
        or type(value.get("version")) is not int
        or value["version"] != 1
    ):
        raise ArtifactError("unsupported coverage manifest version")
    for name, wanted in expected.items():
        if value.get(name) != wanted:
            raise ArtifactError(f"coverage provenance mismatch: {name}")
    root = value.get("producer_root")
    if (
        not isinstance(root, str)
        or not PurePosixPath(root).is_absolute()
        or ".." in PurePosixPath(root).parts
    ):
        raise ArtifactError("invalid producer checkout root")
    if not isinstance(value.get("producer_attempt"), str) or not value["producer_attempt"]:
        raise ArtifactError("invalid producing attempt")
    digests = value.get("files")
    if not isinstance(digests, dict) or set(digests) != set(FILES):
        raise ArtifactError("invalid coverage file digest manifest")
    for name in FILES:
        if digests[name] != _digest(artifact_dir / name):
            raise ArtifactError(f"coverage digest mismatch: {name}")
    return value


def import_report(
    artifact_dir: Path,
    destination: Path,
    *,
    commit: str,
    run_id: str,
    consumer_root: Path,
    toolchain: Mapping[str, str],
    configuration: str = "coverage",
    min_files: int = 100,
) -> Path:
    """Verify identity/digests, relocate section headers, and validate import paths."""
    try:
        if artifact_dir.resolve() == destination.resolve():
            raise ArtifactError("coverage import destination must differ from its artifact")
        if min_files < 100:
            raise ArtifactError("coverage import requires at least 100 resolvable sections")
        destination.mkdir(parents=True, exist_ok=True)
        report_path = destination / FILES[0]
        report_path.unlink(missing_ok=True)
        (destination / FILES[1]).unlink(missing_ok=True)
        value = _manifest(
            artifact_dir,
            {
                "commit": commit,
                "workflow_run_id": run_id,
                "configuration": configuration,
                "toolchain": dict(toolchain),
            },
        )
        report, _ = _read_report(artifact_dir)
        producer = PurePosixPath(value["producer_root"])
        consumer = consumer_root.resolve()
        relocated = []
        resolvable = 0

        def section(match):
            nonlocal resolvable
            original = PurePosixPath(match.group(1))
            if original.is_relative_to(producer):
                relative = original.relative_to(producer)
                if ".." in relative.parts:
                    raise ArtifactError("unsafe coverage section path")
                path = consumer / relative
                if path.is_file() and path.resolve().is_relative_to(consumer):
                    resolvable += 1
                header = path.as_posix()
            else:
                header = str(original)
            relocated.append(header)
            return header + ":"

        report = SECTION.sub(section, report)
        if resolvable < min_files:
            raise ArtifactError(
                f"coverage has {resolvable} resolvable project sections; "
                f"expected at least {min_files}"
            )
        with tempfile.TemporaryDirectory(prefix=".import-", dir=destination) as temporary:
            pending = Path(temporary) / FILES[0]
            pending.write_text(report)
            (destination / FILES[1]).write_text("\n".join(relocated) + "\n")
            pending.replace(report_path)
        return report_path
    except (OSError, json.JSONDecodeError) as error:
        raise ArtifactError(f"cannot import coverage: {error}") from error


def toolchain_identity() -> dict[str, str]:
    """Record only relevant tool versions; credentials and environment stay out."""
    versions = {}
    for tool in ("clang", "llvm-cov", "llvm-profdata"):
        if tool == "clang":
            command = [tool]
        else:
            default = "xcrun " + tool if sys.platform == "darwin" else tool
            command = shlex.split(os.environ.get(tool.upper().replace("-", "_"), default))
        result = subprocess.run([*command, "--version"], capture_output=True, text=True, check=True)
        versions[tool] = result.stdout.strip()
        if not versions[tool]:
            raise ArtifactError(f"cannot identify coverage tool: {tool}")
    return versions


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    for name in ("export", "import"):
        command = subparsers.add_parser(name)
        command.add_argument("--commit", required=True)
        command.add_argument("--run-id", required=True)
        command.add_argument("--output", type=Path, required=True)
        if name == "export":
            command.add_argument("--coverage-dir", type=Path, required=True)
            command.add_argument("--producer-root", type=Path, required=True)
            command.add_argument("--attempt", required=True)
        else:
            command.add_argument("--artifact-dir", type=Path, required=True)
            command.add_argument("--consumer-root", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        common = {"commit": args.commit, "run_id": args.run_id, "toolchain": toolchain_identity()}
        if args.operation == "export":
            result = export_report(
                args.coverage_dir,
                args.output,
                attempt=args.attempt,
                producer_root=args.producer_root,
                **common,
            )
        else:
            result = import_report(
                args.artifact_dir, args.output, consumer_root=args.consumer_root, **common
            )
        print(result)
        return 0
    except (ArtifactError, OSError, subprocess.SubprocessError) as error:
        print(f"Sonar coverage: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
