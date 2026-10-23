"""Validate a Bazel-built analysis artifact against its selected LLVM host."""

import hashlib
import json
import os
import re
import subprocess
import sys
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

REQUIRED_LLVM_VERSION = "23.1.4"
REQUIRED_CHECKS = frozenset(
    {
        "fastecu-accumulate-loop",
        "fastecu-aggregate-parens",
        "fastecu-any-all-index-loop",
        "fastecu-assertion-loop-trace",
        "fastecu-container-assertion",
        "fastecu-count-if-loop",
        "fastecu-linear-search-loop",
        "fastecu-message-concatenation",
        "fastecu-parameterized-case-name",
        "fastecu-plan-helper",
        "fastecu-redundant-address-dereference",
        "fastecu-result-access",
        "fastecu-result-assertion",
        "fastecu-single-use-result",
        "fastecu-template-typename",
        "fastecu-transform-collect-loop",
        "fastecu-uniform-bytes",
    }
)


class ToolchainError(RuntimeError):
    """The analysis host and compiled checks are incompatible or incomplete."""


@dataclass(frozen=True)
class AnalysisHost:
    executable: Path
    arguments: tuple[str, ...]
    llvm_root: Path | None = None


def runfiles_analysis_host(environ: Mapping[str, str]) -> AnalysisHost:
    directory = Path(__file__).parent.parent / "plugin"
    artifact = directory / (
        "fastecu_clang_tidy.exe"
        if os.name == "nt"
        else "libfastecu_tidy.dylib"
        if sys.platform == "darwin"
        else "libfastecu_tidy.so"
    )
    try:
        identity = json.loads((directory / "llvm-identity.json").read_text())
        root = Path(environ.get("FASTECU_LLVM_ROOT") or identity["root"])
    except (OSError, ValueError, KeyError) as error:
        raise ToolchainError(
            f"missing LLVM runfiles identity; use the Bazel analysis target: {error}"
        ) from error
    return resolve_analysis_host(root, artifact)


def resolve_analysis_host(llvm_root: Path, artifact: Path) -> AnalysisHost:
    root = llvm_root.resolve()
    if not artifact.is_file():
        raise ToolchainError(
            f"missing analysis artifact: {artifact}; build the Bazel analysis target"
        )
    try:
        identity = json.loads((artifact.parent / "llvm-identity.json").read_text())
    except (OSError, ValueError) as error:
        raise ToolchainError(f"missing or invalid LLVM build identity: {error}") from error
    if not isinstance(identity, dict) or not isinstance(identity.get("root"), str):
        raise ToolchainError("invalid LLVM build identity: expected an installation root")
    artifact = artifact.resolve()
    if identity.get("version") != REQUIRED_LLVM_VERSION:
        raise ToolchainError(
            f"analysis requires LLVM {REQUIRED_LLVM_VERSION}; rebuild with that installation"
        )
    if Path(identity["root"]).resolve() != root:
        raise ToolchainError(
            "analysis artifact was built against another LLVM installation; rebuild it"
        )
    if not (root / "include/clang-tidy/ClangTidyModule.h").is_file():
        raise ToolchainError(f"missing clang-tidy development headers in {root}")
    linked = artifact.suffix == ".exe"
    executable = artifact if linked else root / "bin/clang-tidy"
    arguments = () if linked else (f"--load={artifact}",)
    if not executable.is_file():
        raise ToolchainError(f"missing analysis executable: {executable}")
    expected_hash = identity.get("clang_tidy_sha256")
    original_host = root / "bin" / ("clang-tidy.exe" if linked else "clang-tidy")
    if expected_hash and hashlib.sha256(original_host.read_bytes()).hexdigest() != expected_hash:
        raise ToolchainError(
            "LLVM executable changed since the artifact build; rebuild the analysis target"
        )
    try:
        version = subprocess.check_output(
            [str(executable), "--version"], text=True, stderr=subprocess.STDOUT
        )
        if not re.search(r"\b" + re.escape(REQUIRED_LLVM_VERSION) + r"\b", version):
            raise ToolchainError(f"analysis executable must report LLVM {REQUIRED_LLVM_VERSION}")
        registered = subprocess.check_output(
            [str(executable), *arguments, "--config={}", "--checks=-*,fastecu-*", "--list-checks"],
            text=True,
            stderr=subprocess.STDOUT,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise ToolchainError(f"cannot load the repository checks: {error}") from error
    if missing := REQUIRED_CHECKS - set(registered.split()):
        raise ToolchainError(
            "analysis host did not register required repository checks: "
            + ", ".join(sorted(missing))
        )
    return AnalysisHost(executable, arguments, root)
