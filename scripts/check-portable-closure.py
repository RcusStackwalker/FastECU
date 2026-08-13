#!/usr/bin/env python3
"""Fail when a portable target is missing or declares a Qt/JNI dependency.

Removing QT_DEPS from a portable target is self-enforcing: Bazel's sandbox
drops Qt off the include path, so a residual `#include <QByteArray>` fails to
compile. This check covers only what the compiler cannot see -- a portable
target that reaches Qt transitively by depending on a :qt_compat sibling.

It also enforces the stronger, package-level promise that step 5e made for
//src/backend/flash and //src/backend/checksum: those two packages must hold
no Qt rule whatsoever (see QT_FREE_PACKAGES below).

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
        "logger_definition_model",
        "logger_definition_parser",
        "logging_types",
        "logging_session",
        "logging_conversion",
        "logging_use_case",
        "logger_conf",
        "logger_definition_service",
    },
    ROOT / "src/backend/logging/protocols": {"protocols"},
    ROOT / "src/backend/protocol": {"protocol"},
    ROOT / "src/backend/flash": {
        "flash_types",
        "flash_plan",
        "flash_validation",
        "flash_executor",
        "flash_device_lookup",
    },
    ROOT / "src/backend/flash/eeprom": {
        "denso_sh705x_eeprom_common",
        "eeprom_read_plan",
        "denso_sh705x_eeprom_kline",
        "denso_sh705x_eeprom_can",
    },
    ROOT / "src/backend/flash/ecu": {
        "mitsu_colt_m32r_can_plan",
        "mitsu_colt_m32r_can_executor",
        "subaru_mitsu_m32r_kline_plan",
        "subaru_mitsu_m32r_kline_executor",
        "subaru_denso_mc68hc16y5_02_plan",
        "subaru_denso_mc68hc16y5_02_executor",
        "subaru_denso_sh7055_02_plan",
        "subaru_denso_sh7055_02_executor",
    },
    ROOT / "src/backend/config": {
        "config_paths",
        "app_config",
        "protocol_catalog",
        "car_model_catalog",
        "provisioning",
    },
    ROOT / "src/backend/checksum": {
        "checksum_selection",
        "dispatch",
    },
    ROOT / "src/backend/definition": {
        "definition_model",
        "parser_utils",
        "romraider_parser",
        "ecuflash_parser",
        "definition_resolver",
        "definition_service",
        "definition_writer",
        "text_format",
    },
    ROOT / "src/backend/calibration": {"calibration_service"},
}

FORBIDDEN = (
    re.compile(r'"@rules_qt//'),
    re.compile(r"QT_DEPS"),
    re.compile(r':qt_compat"'),
    re.compile(r'"@bazel_tools//tools/jdk:jni"'),
)

# Packages that step 5e emptied of Qt entirely, and whose 5e completion
# criterion is "carries no QT_DEPS" -- see the step 5e design doc.
#
# Why this is needed on top of the per-target FORBIDDEN scan: portable_targets()
# below deliberately skips `qt_cc_library(...)` invocations (its `(?<!\w)`
# lookbehind exists for exactly that), so re-adding a Qt target to one of these
# packages -- precisely the regression 5e exists to prevent -- would sail past
# the per-target scan, which only ever inspects plain `cc_library` bodies.
#
# Why an explicit two-package list and NOT a blanket rule over PORTABLE_ROOTS:
# src/backend/logging and src/backend/definition are also portable roots but
# legitimately hold Qt adapter/test targets alongside their portable ones, so
# banning Qt package-wide would be false there. Only these two packages made
# the stronger "no Qt at all" promise. Add a package here only when it has
# actually been emptied of Qt.
QT_FREE_PACKAGES = (
    ROOT / "src/backend/flash",
    ROOT / "src/backend/checksum",
)

QT_FREE_FORBIDDEN = (
    re.compile(r"(?<!\w)qt_cc_library"),
    re.compile(r"QT_DEPS"),
    re.compile(r'"@rules_qt//'),
)

# Comment bodies are stripped before the QT_FREE_FORBIDDEN scan so that a
# comment *explaining* the ban (like the ones this check invites) is not itself
# a violation.
COMMENT_RE = re.compile(r"#[^\n]*")

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
    would misidentify the legacy Qt macro invocations that share packages
    with the new flash/eeprom portable targets as portable cc_library rules.
    """
    for m in re.finditer(
        r'(?<!\w)cc_library\(\s*name = "(?P<name>[^"]+)",(?P<body>.*?)\n\)', text, re.S
    ):
        if m.group("name").endswith("qt_compat"):
            continue
        yield m.group("name"), m.group("body")


def qt_free_package_errors():
    """Return an error list for any QT_FREE_PACKAGES BUILD file that mentions Qt.

    Each package's BUILD.bazel must already be `data`-listed on the
    //:portable_closure target (both of the current entries are, via their
    `exports_files(["BUILD.bazel"])`); a missing file is reported as a failure
    rather than silently passing.
    """
    errors = []
    for package in QT_FREE_PACKAGES:
        build = package / "BUILD.bazel"
        rel = build.relative_to(ROOT)
        if not build.is_file():
            errors.append(f"  {rel}: Qt-free package BUILD.bazel not found")
            continue
        text = COMMENT_RE.sub("", build.read_text())
        for pattern in QT_FREE_FORBIDDEN:
            for match in pattern.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                errors.append(
                    f"  {rel}:{line}: Qt-free package must contain no Qt rules or "
                    f"deps, but matches {pattern.pattern}"
                )
    return errors


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

    errors += qt_free_package_errors()

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
        print("\nPortable targets must not declare QT_DEPS, @rules_qt//..., a")
        print(":qt_compat dep, or JNI. Put Qt overloads in the sibling")
        print(":qt_compat target and have the consumer depend on that instead.")
        print(
            "\nThe Qt-free packages ("
            + ", ".join(f"//{p.relative_to(ROOT)}" for p in QT_FREE_PACKAGES)
            + ") must additionally contain no qt_cc_library rule at all: put the"
        )
        print("Qt-facing code in src/platform or src/ui instead.")
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
    print(
        "Qt-free packages (no qt_cc_library, no QT_DEPS): "
        + ", ".join(f"//{p.relative_to(ROOT)}" for p in QT_FREE_PACKAGES)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
