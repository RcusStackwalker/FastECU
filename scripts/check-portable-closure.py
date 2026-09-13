#!/usr/bin/env python3
"""Fail when a portable target is missing or declares a JNI dependency.

Every route to Qt is closed by the build graph rather than by this file: the
//bazel/qt:* aliases and the qt_compat / legacy shim packages are gated by the
//bazel/qt:qt_layer package group, which holds no portable package, and
@rules_qt is not in this module's repo mapping at all (see
third_party/qt/MODULE.bazel), so its labels do not resolve anywhere in FastECU.

What is left for a check to do is JNI, which has no such wrapper, and the two
registry assertions: every portable target named in bazel/portable_targets.bzl
exists, and nothing under //src/platform is reachable from one.

See docs/adr/0016-enforce-qt-reachability-by-visibility.md.
"""

import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Bazel's py_test runner invokes this file from inside its own runfiles tree
# (".../portable_closure.runfiles/_main/scripts/check-portable-closure.py").
# `ROOT`, above, is *resolved* -- it follows the runfiles symlink for this
# source file back to its canonical location in the workspace, which is right
# for reading `data`-listed source files like BUILD.bazel (their runfiles
# symlink points at that same canonical source-tree copy, so the content is
# identical either way). A `genquery` output has no such canonical
# source-tree copy -- it only exists as a generated file under bazel-out --
# so resolving through the symlink lands nowhere useful. For that file we
# instead use the *unresolved* runfiles path, which does contain it.
RUNFILES_ROOT = Path(__file__).parent.parent

REGISTRY_ENV = "PORTABLE_REGISTRY"


def read_registry():
    """Return {package: [required target names]} from the environment.

    BUILD.bazel passes bazel/portable_targets.bzl's PORTABLE_PACKAGES as JSON
    on this test's `env`, so the registry the genquery closure roots are built
    from and the registry checked here cannot drift apart. An empty name list
    means the whole package is portable: every cc_library in it is scanned and
    none is individually required.
    """
    raw = os.environ.get(REGISTRY_ENV)
    if not raw:
        print(
            f"FAIL: {REGISTRY_ENV} is not set -- this check reads its registry from "
            "the env that //:portable_closure sets; run it through `bazel test`"
        )
        return None
    return json.loads(raw)


# What the build graph cannot reach on its own. Every Qt route is closed: the
# //bazel/qt:* aliases and the qt_compat / legacy shim packages are gated by
# //bazel/qt:qt_layer, which holds no portable package, and @rules_qt is not in
# this module's repo mapping at all, so its labels do not resolve. JNI has no
# such wrapper, and @bazel_tools is visible to every module.
FORBIDDEN = (re.compile(r'"@bazel_tools//tools/jdk:jni"'),)

# Any label under this prefix reached transitively from a required backend
# target means a portable target depends on platform code -- directly or
# indirectly -- which the per-BUILD-file regex scan above cannot see, since
# that scan only inspects each target's own immediate BUILD-file text, not
# its resolved transitive closure.
PLATFORM_LABEL_PREFIX = "//src/platform/"

# Bazel's genquery output filename is exactly the rule name (no extension);
# for a root-package target like `:portable_backend_closure`, that output is
# placed directly under the runfiles root as data-dependencies of this test.
GENQUERY_OUTPUT = RUNFILES_ROOT / "portable_backend_closure"


def read_genquery_output():
    """Return the label list from the portable_backend_closure genquery.

    Returns None (with a printed FAIL) if the genquery output file cannot be
    found -- e.g. the `data` wiring in BUILD.bazel is broken -- so that a
    silent absence never gets misread as "no platform labels found".
    """
    if not GENQUERY_OUTPUT.is_file():
        print(
            "FAIL: portable_backend_closure genquery output not found at "
            f"{GENQUERY_OUTPUT} -- check the `data` entry "
            "for ':portable_backend_closure' on the //:portable_closure target"
        )
        return None
    return [line for line in GENQUERY_OUTPUT.read_text().splitlines() if line.strip()]


def portable_targets(text):
    """Yield (name, body) for every cc_library whose name lacks a qt_compat suffix.

    The `(?<!\\w)` lookbehind keeps this from matching inside `qt_cc_library(`
    -- "cc_library(" is a substring of "qt_cc_library(", so without it this
    would misidentify a Qt macro invocation as a portable cc_library rule. No
    scanned package still invokes that macro (src/backend cannot load it; see
    docs/adr/0016-enforce-qt-reachability-by-visibility.md), but the
    lookbehind stays so that reintroducing one cannot silently pass.

    A root with an explicit required-target set is filtered against that set by
    the caller: its package also holds Qt-linked `Legacy*Adapter` targets,
    which are plain cc_library rules since 0016 and are portable only in the
    "no widgets, no moc" sense this file does not police.
    """
    for m in re.finditer(
        r'(?<!\w)cc_library\(\s*name = "(?P<name>[^"]+)",(?P<body>.*?)\n\)', text, re.S
    ):
        if m.group("name").endswith("qt_compat"):
            continue
        yield m.group("name"), m.group("body")


def main():
    registry = read_registry()
    if registry is None:
        return 1

    build_files = []
    required_by_build = {}
    for package, required_targets in registry.items():
        build = ROOT / package / "BUILD.bazel"
        if not build.is_file():
            print(f"FAIL: registered package has no BUILD.bazel: {package}")
            return 1
        build_files.append(build)
        if required_targets:
            required_by_build[build] = set(required_targets)

    build_files = sorted(set(build_files))
    errors = []
    checked = 0
    for build in build_files:
        text = build.read_text()
        rel = build.relative_to(ROOT)
        found_any = False
        targets = list(portable_targets(text))
        names = {name for name, _ in targets}
        for missing in sorted(required_by_build.get(build, set()) - names):
            errors.append(f"  {rel}: required portable target '{missing}' is missing")
        required = required_by_build.get(build)
        for name, body in targets:
            found_any = True
            if required is not None and name not in required:
                continue
            checked += 1
            for pattern in FORBIDDEN:
                if pattern.search(body):
                    errors.append(f"  {rel}: portable target '{name}' matches {pattern.pattern}")
        if not found_any and build not in required_by_build:
            errors.append(f"  {rel}: no portable cc_library found")

    closure_labels = read_genquery_output()
    if closure_labels is None:
        return 1
    for label in closure_labels:
        if label.startswith(PLATFORM_LABEL_PREFIX):
            errors.append(
                "  portable_backend_closure: required backend target transitively "
                f"depends on platform label {label}"
            )

    if errors:
        print("FAIL: portable closure requirements were violated:")
        print("\n".join(errors))
        print("\nPortable targets must not reach Qt or JNI. Put Qt overloads in")
        print("the package's qt_compat or legacy sibling package and have the")
        print("consumer depend on that instead.")
        return 1

    required_count = sum(len(targets) for targets in required_by_build.values())
    required_labels = []
    for build, targets in sorted(required_by_build.items()):
        package = build.parent.relative_to(ROOT)
        required_labels.extend(f"//{package}:{name}" for name in sorted(targets))
    print(
        f"OK: checked {checked} portable targets across {len(registry)} packages; "
        f"all {required_count} required targets are present and none reach Qt/JNI."
    )
    print("Required targets: " + ", ".join(required_labels))
    return 0


if __name__ == "__main__":
    sys.exit(main())
