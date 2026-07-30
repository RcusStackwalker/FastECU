#!/usr/bin/env python3
"""Fail when a portable target is missing or declares a Qt/JNI dependency.

Removing QT_DEPS from a portable target is self-enforcing: Bazel's sandbox
drops Qt off the include path, so a residual `#include <QByteArray>` fails to
compile. This check covers only what the compiler cannot see -- a portable
target that reaches Qt transitively by depending on a :qt_compat sibling.

Rejects Qt and JNI. @openssl is deliberately ALLOWED; step 7 decides its fate.
"""

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

PORTABLE_ROOTS = {
    ROOT / "src/algorithms": None,
    ROOT / "src/backend/ports": {"ports"},
    ROOT / "src/backend/logging": {
        "logging_types",
        "logging_session",
        "logging_conversion",
        "logging_use_case",
    },
    ROOT / "src/backend/logging/protocols": {"protocols"},
    ROOT / "src/backend/protocol": {"protocol"},
    ROOT / "src/backend/flash": {
        "flash_types",
        "flash_plan",
        "flash_validation",
        "flash_executor",
    },
    ROOT / "src/backend/flash/eeprom": {
        "denso_sh705x_eeprom_common",
        "denso_sh705x_eeprom_kline",
        "denso_sh705x_eeprom_can",
    },
    ROOT / "src/backend/config": {
        "config_paths",
        "app_config",
        "protocol_catalog",
        "provisioning",
    },
    ROOT / "src/backend/checksum": {
        "checksum_selection",
        "flash_device_lookup",
        "dispatch",
    },
    ROOT / "src/backend/definition": {
        "definition_model",
    },
}

FORBIDDEN = (
    re.compile(r'"@rules_qt//'),
    re.compile(r"QT_DEPS"),
    re.compile(r':qt_compat"'),
    re.compile(r'"@bazel_tools//tools/jdk:jni"'),
)

# Any label under this prefix reached transitively from a required flash
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
    would misidentify the legacy Qt macro invocations that share packages
    with the new flash/eeprom portable targets as portable cc_library rules.
    """
    for m in re.finditer(
        r'(?<!\w)cc_library\(\s*name = "(?P<name>[^"]+)",(?P<body>.*?)\n\)', text, re.S
    ):
        if m.group("name").endswith("qt_compat"):
            continue
        yield m.group("name"), m.group("body")


def main():
    build_files = []
    required_by_build = {}
    for root, required_targets in PORTABLE_ROOTS.items():
        if required_targets is None:
            # `None` marks a root whose *entire* subtree is portable (e.g.
            # src/algorithms) -- recurse into every package under it.
            found = sorted(root.rglob("BUILD.bazel"))
        else:
            # A root with an explicit required-target set may share its
            # directory tree with legacy, intentionally-non-portable
            # sibling packages (e.g. src/backend/flash/{bdm,bootmode,ecu,
            # jtag,tcu} still hold Qt-only targets). Only the root's own
            # BUILD.bazel is in scope; a sibling package that also needs
            # checking gets its own explicit PORTABLE_ROOTS entry (as
            # src/backend/flash/eeprom does).
            root_build = root / "BUILD.bazel"
            found = [root_build] if root_build.is_file() else []
        if not found:
            print(f"FAIL: no BUILD.bazel files found under {root}")
            return 1
        build_files += found
        if required_targets is not None:
            root_build = root / "BUILD.bazel"
            if root_build not in found:
                print(f"FAIL: required BUILD.bazel is missing: {root_build.relative_to(ROOT)}")
                return 1
            required_by_build[root_build] = required_targets

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
        for name, body in targets:
            found_any = True
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
                "  portable_backend_closure: required flash target transitively "
                f"depends on platform label {label}"
            )

    if errors:
        print("FAIL: portable closure requirements were violated:")
        print("\n".join(errors))
        print("\nPortable targets must not declare QT_DEPS, @rules_qt//..., a")
        print(":qt_compat dep, or JNI. Put Qt overloads in the sibling")
        print(":qt_compat target and have the consumer depend on that instead.")
        return 1

    required_count = sum(len(targets) for targets in PORTABLE_ROOTS.values() if targets is not None)
    required_labels = []
    for build, targets in sorted(required_by_build.items()):
        package = build.parent.relative_to(ROOT)
        required_labels.extend(f"//{package}:{name}" for name in sorted(targets))
    print(
        f"OK: checked {checked} portable targets across {len(PORTABLE_ROOTS)} roots; "
        f"all {required_count} required targets are present and none reach Qt/JNI."
    )
    print("Required targets: " + ", ".join(required_labels))
    return 0


if __name__ == "__main__":
    sys.exit(main())
