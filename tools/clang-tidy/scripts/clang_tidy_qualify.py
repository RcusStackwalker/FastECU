#!/usr/bin/env python3
"""Temporary migration qualification; removed with the compilation-database runner."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from functools import cache
from pathlib import Path

import clang_tidy_aspect as aspect
import clang_tidy_parity as parity
import clang_tidy_runner as legacy
import clang_tidy_scope as scope
import clang_tidy_toolchain as toolchain
import yaml


def action_configurations(
    workspace: Path, targets: list[str], sources: set[str], *, lint: bool
) -> list:
    mnemonic = "AspectRulesLintClangTidy" if lint else "CppCompile"
    command = ["bazel", "aquery", "--config=release", "--output=jsonproto"]
    if lint:
        command.extend([f"--aspects={aspect.ASPECT}", f"--output_groups={aspect.GROUPS}"])
    command.append(f'mnemonic("{mnemonic}", deps(set({" ".join(targets)})))')
    result = subprocess.run(command, cwd=workspace, capture_output=True, text=True, check=True)
    query = json.loads(result.stdout)
    fragments = {item["id"]: item for item in query["pathFragments"]}
    artifacts = {item["id"]: item["pathFragmentId"] for item in query["artifacts"]}
    sets = {item["id"]: item for item in query["depSetOfFiles"]}
    labels = {item["id"]: item["label"] for item in query["targets"]}
    configurations = {item["id"]: item["checksum"] for item in query["configuration"]}

    @cache
    def path(identifier: int) -> str:
        fragment = fragments[identifier]
        parent = fragment.get("parentId")
        return (path(parent) + "/" if parent else "") + fragment["label"]

    @cache
    def inputs(identifier: int) -> frozenset[str]:
        item = sets[identifier]
        direct = {path(artifacts[value]) for value in item.get("directArtifactIds", [])}
        return frozenset(direct).union(
            *(inputs(value) for value in item.get("transitiveDepSetIds", []))
        )

    records = set()
    for action in query["actions"]:
        files = frozenset().union(*(inputs(value) for value in action.get("inputDepSetIds", [])))
        for source in files & sources:
            records.add(
                (source, labels[action["targetId"]], configurations[action["configurationId"]])
            )
    return sorted(records)


def run_aspect(
    workspace: Path, output: Path, label: str, targets: list[str], prefixes: tuple
) -> dict:
    log_path = output / f"{label}-aspect.log"
    start = time.monotonic()
    with tempfile.TemporaryDirectory() as temporary, log_path.open("w", encoding="utf-8") as log:
        events = Path(temporary) / "events.json"
        command = aspect.build_command(targets, events, ["--config=release", *aspect.DOWNLOAD_ARGS])
        result = subprocess.run(
            command, cwd=workspace, stdout=log, stderr=subprocess.STDOUT, check=False
        )
        files = aspect.select_outputs(aspect.output_files(events), prefixes)
        findings = aspect.report(
            files, workspace=workspace, write=log.write, color=False, annotate=False
        )
        with contextlib.redirect_stdout(log):
            aspect.emit_profile(files, {})
        metrics = [
            json.loads(line)["buildMetrics"]
            for line in events.read_text().splitlines()
            if "buildMetrics" in json.loads(line)
        ]
    if result.returncode or not aspect.source_count(files):
        raise RuntimeError(f"{label} aspect failed: {result.returncode}; see {log_path}")
    sources = sorted({files.sources[path] for path in files.human if path in files.sources})
    return {
        "seconds": time.monotonic() - start,
        "sources": sources,
        "findings": findings,
        "build_metrics": metrics,
    }


def run_legacy(
    workspace: Path,
    output: Path,
    label: str,
    compdb: str,
    scope_os: str | None,
    host: toolchain.AnalysisHost,
) -> dict:
    start = time.monotonic()
    log_path = output / f"{label}-legacy.log"
    changed = ["tests/clang_tidy_qualification/probe.cpp"] if label == "seeded" else []
    if changed and sys.platform == "win32":
        changed.append("src/platform/desktop/windows/j2534/pe_bitness.cpp")

    def commands(command: list[str], **kwargs) -> subprocess.CompletedProcess:
        # Supply the actual seed edits as a controlled Git input. The migration's
        # committed enforcement diff must not widen this focused detection probe.
        if changed and command[:2] == ["git", "merge-base"]:
            return subprocess.CompletedProcess(command, 0, "HEAD\n", "")
        if changed and command[:2] == ["git", "diff"]:
            return subprocess.CompletedProcess(command, 0, "\n".join(changed) + "\n", "")
        if changed and command[:2] == ["git", "ls-files"]:
            return subprocess.CompletedProcess(command, 0, "", "")
        return subprocess.run(command, **kwargs)

    with log_path.open("w", encoding="utf-8", buffering=1) as log, contextlib.redirect_stdout(log):
        try:
            legacy.run_workflow(
                mode="report",
                workspace=workspace,
                compdb_tool=compdb,
                platform_name=sys.platform,
                environ=os.environ,
                build_args=["--config=release", "//..."],
                compdb_args=["--config=release"],
                scope_os=scope_os,
                jobs=4,
                analysis_host=host,
                changed=bool(changed),
                command_runner=commands,
            )
        except legacy.WorkflowError as error:
            print(error)
            log.flush()
            if not parity.findings(log_path.read_text().splitlines()):
                raise
    entries = legacy.load_project_entries(workspace, workspace / "compile_commands.json")
    tree = legacy._workspace_tree(workspace)
    prefixes = scope.scope_prefixes(workspace, scope_os)
    sources = sorted(
        {
            relative.as_posix()
            for entry in entries
            if (relative := legacy._entry_relative_path(entry, tree)) is not None
            and (not prefixes or scope.in_scope(relative, prefixes))
        }
    )
    return {
        "seconds": time.monotonic() - start,
        "sources": sources,
        "analyzed_sources": changed or sources,
        "controlled_git_input": bool(changed),
        "parallel_jobs": 4,
    }


def compare(
    workspace: Path,
    output: Path,
    label: str,
    compdb: str,
    scope_os: str | None,
    host: toolchain.AnalysisHost,
    *,
    reference: dict | None = None,
) -> dict:
    old = (
        reference["legacy"]
        if reference
        else run_legacy(workspace, output, label, compdb, scope_os, host)
    )
    prefixes = scope.scope_prefixes(workspace, scope_os)
    targets = scope.build_targets(prefixes) if prefixes else ["//..."]
    new = run_aspect(workspace, output, label, targets, prefixes)
    expected = (
        {tuple(item) for item in reference["findings"]}
        if reference
        else parity.findings((output / f"{label}-legacy.log").read_text().splitlines())
    )
    actual = parity.findings((output / f"{label}-aspect.log").read_text().splitlines())
    details = {
        "legacy": old,
        "aspect": new,
        "only_legacy_sources": sorted(set(old["sources"]) - set(new["sources"])),
        "only_aspect_sources": sorted(set(new["sources"]) - set(old["sources"])),
        "finding_differences": parity.compare(expected, actual),
        "findings": sorted(actual),
        "compile_configurations": action_configurations(
            workspace, targets, set(old["sources"]), lint=False
        ),
        "aspect_configurations": action_configurations(
            workspace, targets, set(new["sources"]), lint=True
        ),
    }
    (output / f"{label}.json").write_text(json.dumps(details, indent=2) + "\n")
    if reference and details["compile_configurations"] != [
        tuple(item) for item in reference["compile_configurations"]
    ]:
        raise RuntimeError("the compiled source/configuration set changed since the reference")
    if (
        details["only_legacy_sources"]
        or details["only_aspect_sources"]
        or details["finding_differences"]
    ):
        raise RuntimeError(f"{label} parity differs; see {output / (label + '.json')}")
    if details["compile_configurations"] != details["aspect_configurations"]:
        raise RuntimeError(
            f"{label} configured action sets differ; see {output / (label + '.json')}"
        )
    if not details["compile_configurations"]:
        raise RuntimeError(f"{label} configured action sets are empty")
    return details


@contextlib.contextmanager
def seeded_sources(workspace: Path):
    roots = (
        workspace / "tests/clang_tidy_qualification",
        workspace / "tools/clang-tidy/qualification",
    )
    source = """#include "tools/clang-tidy/qualification/probe.hpp"
namespace fastecu::qualification
{
template <class T> struct Probe { T value; };
void SourceProbe(int& value)
{
    if (value > 0)
        HeaderProbe(value);
}
}
"""
    header = """#pragma once
namespace fastecu::qualification
{
inline void HeaderProbe(int& value)
{
    if (value > 0)
        value += 1;
}
}
"""
    files = {
        roots[0] / "probe.cpp": source,
        roots[0] / "BUILD.bazel": (
            'load("@rules_cc//cc:defs.bzl", "cc_library")\n'
            'cc_library(name="probe", srcs=["probe.cpp"],\n'
            '    hdrs=["//tools/clang-tidy/qualification:probe.hpp"])\n'
        ),
        roots[1] / "probe.hpp": header,
        roots[1] / "BUILD.bazel": (
            'exports_files(["probe.hpp"],\n'
            '    visibility=["//tests/clang_tidy_qualification:__pkg__"])\n'
        ),
    }
    original = {}
    try:
        for path, content in files.items():
            if path.exists():
                raise RuntimeError(f"qualification probe already exists: {path}")
            original[path] = None
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        if sys.platform == "win32":
            windows = workspace / "src/platform/desktop/windows/j2534/pe_bitness.cpp"
            original[windows] = windows.read_bytes()
            windows.write_bytes(
                original[windows]
                + b"\nnamespace fastecu::qualification {\n"
                + b"template <class T> struct WindowsProbe { T value; }; }\n"
            )
        yield
    finally:
        for path, content in original.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(content)
        for root in roots:
            if root.exists() and not any(root.iterdir()):
                root.rmdir()


def clean_reference(reference: Path, workspace: Path) -> dict:
    """Reuse a legacy baseline only when its source and build inputs are unchanged."""
    metadata = json.loads((reference.parent / "metadata.json").read_text())
    revision = metadata["revision"]
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise RuntimeError("invalid clean reference revision")
    known = subprocess.run(
        ["git", "cat-file", "-e", revision], cwd=workspace, capture_output=True, check=False
    )
    if known.returncode:
        subprocess.run(["git", "fetch", "origin", revision], cwd=workspace, check=True)
    changed = subprocess.check_output(
        ["git", "diff", "--name-only", metadata["revision"], "HEAD"], cwd=workspace, text=True
    ).splitlines()
    allowed = {
        "tools/clang-tidy/scripts/clang_tidy_qualify.py",
        "tools/clang-tidy/scripts/clang_tidy_aspect.py",
        "tools/clang-tidy/scripts/tests/clang_tidy_aspect_test.py",
    }
    manifest = ".clang-tidy-scope.toml"
    if manifest in changed:
        original = subprocess.check_output(
            ["git", "show", f"{revision}:{manifest}"], cwd=workspace, text=True
        )
        current = "".join(
            line
            for line in (workspace / manifest).read_text().splitlines(keepends=True)
            if line.strip() != '"tools/clang-tidy/aspect/tests",'
        )
        if current != original:
            raise RuntimeError("analysis platform scope changed beyond the manual graph fixture")
        allowed.add(manifest)
    for build_file in ("MODULE.bazel", "bazel/patches/BUILD.bazel"):
        if build_file not in changed:
            continue
        original = subprocess.check_output(
            ["git", "show", f"{revision}:{build_file}"], cwd=workspace, text=True
        )
        current = (workspace / build_file).read_text()
        without_wrapper_patch = "".join(
            line
            for line in current.splitlines(keepends=True)
            if not any(
                f'"{name}"' in line or f'"//bazel/patches:{name}"' in line
                for name in (
                    "rules_lint_clang_tidy_windows_wrapper.patch",
                    "rules_lint_clang_tidy_transitions.patch",
                )
            )
        )
        if without_wrapper_patch != original:
            raise RuntimeError("build inputs changed beyond the analysis aspect patches")
        allowed.add(build_file)
    workflow = ".github/workflows/pr.yml"
    if workflow in changed:
        before = yaml.safe_load(
            subprocess.check_output(
                ["git", "show", f"{metadata['revision']}:{workflow}"], cwd=workspace, text=True
            )
        )
        after = yaml.safe_load((workspace / workflow).read_text())
        for value in (before, after):
            value["permissions"].pop("actions", None)
            job = value["jobs"]["clang-tidy-aspect"]
            job.pop("timeout-minutes", None)
            job["steps"] = [
                step
                for step in job["steps"]
                if step.get("name")
                not in (
                    "Download completed Linux legacy baseline",
                    "Compare clean and seeded clang-tidy scopes",
                )
            ]
        if before != after:
            raise RuntimeError("workflow setup changed since the clean reference")
        allowed.add(workflow)
    if any(
        path not in allowed
        and not path.startswith(
            (
                "docs/",
                "bazel/patches/rules_lint_clang_tidy_",
                "tools/clang-tidy/aspect/tests/",
            )
        )
        for path in changed
    ):
        raise RuntimeError("clean reference predates production or build changes; run clean again")
    if metadata["platform"] != sys.platform or metadata["llvm"] != toolchain.REQUIRED_LLVM_VERSION:
        raise RuntimeError("clean reference uses a different platform or LLVM release")
    details = json.loads(reference.read_text())
    if (
        details["findings"]
        or details["finding_differences"]
        or details["only_legacy_sources"]
        or details["only_aspect_sources"]
        or not details["compile_configurations"]
        or details["compile_configurations"] != details["aspect_configurations"]
    ):
        raise RuntimeError("clean reference does not establish full clean parity")
    return details


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compdb-tool", required=True)
    parser.add_argument("--scope-os")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--smoke", action="store_true", help="check the collector on one target")
    parser.add_argument("--clean-reference", type=Path, help="reuse verified clean evidence")
    args = parser.parse_args()
    workspace = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"]).resolve()
    output = (workspace / args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    host = toolchain.runfiles_analysis_host(os.environ)
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=workspace, text=True
    ).strip()
    (output / "metadata.json").write_text(
        json.dumps(
            {
                "revision": revision,
                "platform": sys.platform,
                "llvm": toolchain.REQUIRED_LLVM_VERSION,
                "host": str(host.executable),
            },
            indent=2,
        )
        + "\n"
    )
    compdb = str(Path(args.compdb_tool).resolve())
    try:
        if args.smoke:
            targets = ["//src/algorithms/expression:expression"]
            sources = {"src/algorithms/expression/expression.cpp"}
            run_aspect(workspace, output, "smoke", targets, ())
            compiled = action_configurations(workspace, targets, sources, lint=False)
            linted = action_configurations(workspace, targets, sources, lint=True)
            if not compiled or compiled != linted:
                raise RuntimeError(f"smoke configuration mismatch: {compiled} versus {linted}")
            with seeded_sources(workspace):
                run_legacy(workspace, output, "seeded", compdb, args.scope_os, host)
                run_aspect(
                    workspace, output, "seeded", ["//tests/clang_tidy_qualification:probe"], ()
                )
                expected = parity.findings((output / "seeded-legacy.log").read_text().splitlines())
                actual = parity.findings((output / "seeded-aspect.log").read_text().splitlines())
                expected = {item for item in expected if "pe_bitness.cpp" not in item[1]}
                if not actual or parity.compare(expected, actual):
                    raise RuntimeError("smoke seeded finding parity failed")
            print("Evidence collector smoke passed for one source/configuration.")
            return 0
        if args.clean_reference:
            baseline = clean_reference(args.clean_reference, workspace)
            clean = compare(
                workspace, output, "clean", compdb, args.scope_os, host, reference=baseline
            )
        else:
            clean = compare(workspace, output, "clean", compdb, args.scope_os, host)
        if clean["findings"]:
            raise RuntimeError("clean scope has findings")
        with seeded_sources(workspace):
            seeded = compare(workspace, output, "seeded", compdb, args.scope_os, host)
            findings = seeded["findings"]
            if not any("fastecu-template-typename" in item[2] for item in findings):
                raise RuntimeError("seeded repository check was not detected")
            if not any("readability-braces-around-statements" in item[2] for item in findings):
                raise RuntimeError("seeded built-in check was not detected")
            if not any("tools/clang-tidy/qualification/probe.hpp" in item[1] for item in findings):
                raise RuntimeError("tools header finding was not detected")
            if sys.platform == "win32" and not any(
                "pe_bitness.cpp" in item[1] for item in findings
            ):
                raise RuntimeError("Windows-exclusive finding was not detected")
        print(
            f"Qualification passed: {len(clean['legacy']['sources'])} clean sources; "
            f"{len(findings)} seeded findings"
        )
        return 0
    except (
        RuntimeError,
        legacy.WorkflowError,
        aspect.AspectError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"qualification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
