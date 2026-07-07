#!/usr/bin/env python3
"""Run clang-tidy in report-only mode over Bazel's source manifest."""

from __future__ import annotations

import ast
import glob
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAX_OUTPUT_LINES_PER_FILE = 120


def load_sources(manifest: Path) -> list[str]:
    values = load_manifest(manifest)
    sources: list[str] = []
    for name, entries in values.items():
        if name.endswith("_SRCS"):
            sources.extend(entry for entry in entries if entry.endswith(".cpp"))
    return sorted(dict.fromkeys(sources))


def load_manifest(manifest: Path) -> dict[str, list[str]]:
    module = ast.parse(manifest.read_text())
    values: dict[str, list[str]] = {}
    for node in module.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name):
            continue
        if not isinstance(node.value, ast.List):
            continue
        values[target.id] = [
            elt.value
            for elt in node.value.elts
            if isinstance(elt, ast.Constant) and isinstance(elt.value, str)
        ]
    return values


def find_clang_tidy() -> str | None:
    candidates = [
        shutil.which("clang-tidy"),
        "/opt/homebrew/opt/llvm/bin/clang-tidy",
        "/usr/local/opt/llvm/bin/clang-tidy",
    ]
    candidates.extend(sorted(glob.glob("/opt/homebrew/Cellar/llvm/*/bin/clang-tidy"), reverse=True))
    candidates.extend(sorted(glob.glob("/usr/local/Cellar/llvm/*/bin/clang-tidy"), reverse=True))
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def qt_include_flags() -> list[str]:
    qmake = shutil.which("qmake6") or shutil.which("qmake")
    include_roots: list[Path] = []
    framework_roots: list[Path] = []
    if qmake:
        result = subprocess.run(
            [qmake, "-query", "QT_INSTALL_HEADERS"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0 and result.stdout.strip():
            include_roots.append(Path(result.stdout.strip()))
        result = subprocess.run(
            [qmake, "-query", "QT_INSTALL_LIBS"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0 and result.stdout.strip():
            framework_roots.append(Path(result.stdout.strip()))

    for candidate in (
        "/opt/homebrew/opt/qt/include",
        "/opt/homebrew/opt/qt6/include",
        "/opt/homebrew/opt/qt@6/include",
        "/usr/local/opt/qt/include",
        "/usr/local/opt/qt6/include",
    ):
        include_roots.append(Path(candidate))

    flags: list[str] = []
    seen: set[str] = set()
    for root in include_roots:
        if not root.is_dir():
            continue
        for include_dir in [root] + [path for path in root.iterdir() if path.is_dir() and path.name.startswith("Qt")]:
            value = str(include_dir)
            if value not in seen:
                flags.append("-I" + value)
                seen.add(value)
    for root in framework_roots:
        if root.is_dir() and any(root.glob("Qt*.framework")):
            value = str(root)
            if value not in seen:
                flags.append("-F" + value)
                seen.add(value)
            for headers in sorted(root.glob("Qt*.framework/Headers")):
                value = str(headers)
                if value not in seen:
                    flags.append("-I" + value)
                    seen.add(value)
    return flags


def qmake_query(key: str) -> str | None:
    qmake = shutil.which("qmake6") or shutil.which("qmake")
    if qmake is None:
        return None
    result = subprocess.run(
        [qmake, "-query", key],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode == 0 and result.stdout.strip():
        return result.stdout.strip()
    return None


def qt_tool(name: str) -> str | None:
    candidates = [
        shutil.which(name),
    ]
    for key in ("QT_HOST_LIBEXECS", "QT_INSTALL_LIBEXECS", "QT_HOST_BINS", "QT_INSTALL_BINS"):
        directory = qmake_query(key)
        if directory:
            candidates.append(str(Path(directory) / name))
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def basename(path: str) -> str:
    return Path(path).stem


def generate_qt_artifacts(manifest_values: dict[str, list[str]], output_dir: Path) -> None:
    moc = qt_tool("moc")
    uic = qt_tool("uic")
    repc = qt_tool("repc")

    if uic:
        for ui in manifest_values.get("APP_FORMS", []):
            subprocess.run(
                [uic, str(ROOT / ui), "-o", str(output_dir / f"ui_{basename(ui)}.h")],
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )

    if repc:
        for rep in manifest_values.get("APP_REPS", []):
            subprocess.run(
                [repc, "-o", "replica", str(ROOT / rep), str(output_dir / f"rep_{basename(rep)}_replica.h")],
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )

    if moc:
        for source in load_sources_from_values(manifest_values):
            path = ROOT / source
            if not path.exists():
                continue
            include_name = f"{path.stem}.moc"
            if f'"{include_name}"' not in path.read_text(errors="ignore"):
                continue
            subprocess.run(
                [moc, str(path), "-o", str(output_dir / include_name)],
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )


def load_sources_from_values(values: dict[str, list[str]]) -> list[str]:
    sources: list[str] = []
    for name, entries in values.items():
        if name.endswith("_SRCS"):
            sources.extend(entry for entry in entries if entry.endswith(".cpp"))
    return sorted(dict.fromkeys(sources))


def openssl_include_flags() -> list[str]:
    flags: list[str] = []
    for candidate in (
        os.environ.get("OPENSSL_ROOT"),
        "/opt/homebrew/opt/openssl@3",
        "/usr/local/opt/openssl@3",
    ):
        if not candidate:
            continue
        include_dir = Path(candidate) / "include"
        if include_dir.is_dir():
            flags.append("-I" + str(include_dir))
            break
    return flags


def print_limited_output(source: str, output: str) -> None:
    lines = output.rstrip().splitlines()
    print(f"===== {source} =====")
    for line in lines[:MAX_OUTPUT_LINES_PER_FILE]:
        print(line)
    if len(lines) > MAX_OUTPUT_LINES_PER_FILE:
        print(f"... truncated {len(lines) - MAX_OUTPUT_LINES_PER_FILE} additional lines for {source}")


def main() -> int:
    manifest = ROOT / (sys.argv[1] if len(sys.argv) > 1 else "bazel/fastecu_sources.bzl")
    clang_tidy = find_clang_tidy()
    if clang_tidy is None:
        print("clang-tidy is not installed; report-only lint step skipped.")
        return 0

    manifest_values = load_manifest(manifest)
    sources = load_sources_from_values(manifest_values)
    with tempfile.TemporaryDirectory(prefix="fastecu-clang-tidy-") as generated:
        generated_dir = Path(generated)
        generate_qt_artifacts(manifest_values, generated_dir)
        include_flags = ["-I" + str(generated_dir)] + qt_include_flags() + openssl_include_flags()
        findings = 0
        for source in sources:
            path = ROOT / source
            if not path.exists():
                continue
            result = subprocess.run(
                [
                    clang_tidy,
                    str(path),
                    "--config-file",
                    str(ROOT / ".clang-tidy"),
                    "--",
                    "-std=c++20",
                    "-DQT_FORCE_ASSERTS",
                    "-I.",
                    "-Iprotocol",
                    "-Iserial_port",
                ] + include_flags,
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if result.stdout.strip():
                findings += 1
                print_limited_output(source, result.stdout)

    if findings:
        print(f"clang-tidy reported findings in {findings} translation units.")
    else:
        print("clang-tidy completed without reportable findings.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
