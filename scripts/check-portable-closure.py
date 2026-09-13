#!/usr/bin/env python3
"""Fail when a portable target is missing or declares a Qt/JNI dependency.

Removing QT_DEPS from a portable target is self-enforcing: Bazel's sandbox
drops Qt off the include path, so a residual `#include <QByteArray>` fails to
compile. This check covers only what the compiler cannot see -- a portable
target that reaches Qt transitively by depending on a :qt_compat sibling.

It covers only the packages visibility cannot. Qt is reached through the
//bazel/qt:* aliases, whose :qt_layer package group excludes every portable
package that holds no Qt at all -- //src/backend/flash and //src/backend/checksum
among them -- so there a Qt dependency fails at analysis. Six packages still
hold a Qt-typed legacy adapter or a :qt_compat shim beside their portable
targets, and Bazel visibility is package-granular, so only a per-target scan
can separate the two. See
docs/adr/0016-enforce-qt-reachability-by-visibility.md.

Rejects Qt and JNI.
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
    ROOT / "src/backend/protocol/uds": {"uds_client"},
    ROOT / "src/backend/service_functions": {
        "service_function_types",
        "service_function_session",
        "tcu_parameter_table",
        "read_parameters_session",
        "set_parameters_session",
        "relearn_session",
    },
    ROOT / "src/backend/flash": {
        "flash_types",
        "flash_plan",
        "flash_validation",
        "flash_executor",
        "flash_device_lookup",
        "can_flash_uds_channel",
    },
    ROOT / "src/backend/flash/eeprom": None,
    ROOT / "src/backend/flash/ecu": {
        "single_window_plan",
        "denso_iso15765_can_common",
        "mitsu_colt_m32r_can_types",
        "mitsu_colt_m32r_can_plan",
        "mitsu_colt_m32r_can_executor",
        "subaru_mitsu_m32r_kline_types",
        "subaru_mitsu_m32r_kline_plan",
        "subaru_mitsu_m32r_kline_executor",
        "subaru_hitachi_m32r_kline_types",
        "subaru_denso_mc68hc16y5_02_types",
        "subaru_denso_mc68hc16y5_02_plan",
        "subaru_denso_mc68hc16y5_02_executor",
        "subaru_denso_sh7055_02_types",
        "subaru_denso_sh7055_02_plan",
        "subaru_denso_sh7055_02_executor",
        "subaru_hitachi_m32r_can_types",
        "subaru_hitachi_m32r_can_plan",
        "subaru_hitachi_m32r_can_executor",
        "subaru_tcu_cvt_hitachi_m32r_can_types",
        "subaru_tcu_cvt_hitachi_m32r_can_plan",
        "subaru_tcu_cvt_hitachi_m32r_can_executor",
        "subaru_tcu_cvt_mitsu_mh8111_can_types",
        "subaru_tcu_cvt_mitsu_mh8111_can_plan",
        "subaru_tcu_cvt_mitsu_mh8111_can_executor",
        "subaru_tcu_cvt_mitsu_mh8104_can_types",
        "subaru_tcu_cvt_mitsu_mh8104_can_plan",
        "subaru_tcu_cvt_mitsu_mh8104_can_executor",
        "subaru_denso_1n83m_1_5m_can_types",
        "subaru_denso_1n83m_1_5m_can_plan",
        "subaru_denso_1n83m_1_5m_can_executor",
        "subaru_denso_sh72531_can_types",
        "subaru_denso_sh72531_can_plan",
        "subaru_denso_sh72531_can_executor",
        "subaru_denso_sh72543_can_diesel_types",
        "subaru_denso_sh72543_can_diesel_plan",
        "subaru_denso_sh72543_can_diesel_executor",
        "subaru_denso_1n83m_4m_can_types",
        "subaru_denso_1n83m_4m_can_plan",
        "subaru_denso_1n83m_4m_can_executor",
    },
    ROOT / "src/backend/config": {
        "config_paths",
        "app_config",
        "protocol_catalog",
        "car_model_catalog",
        "provisioning",
        "menu_definition",
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
    ROOT / "src/backend/calibration": {"calibration_service", "map_edit"},
}

# What visibility cannot reach. A portable target that writes QT_DEPS, a
# //bazel/qt:* alias, or a :qt_compat dep now fails at analysis: Qt lives
# behind aliases and shim packages whose visibility is //bazel/qt:qt_layer,
# and no portable package is in it. These two labels are public in repos we do
# not own, so nothing can narrow them from here.
FORBIDDEN = (
    re.compile(r'"@rules_qt//'),
    re.compile(r'"@bazel_tools//tools/jdk:jni"'),
)

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
        # A package whose only cc_library is a *qt_compat shim holds Qt by
        # design, so "lost all its portable targets" does not apply to it.
        shim_only = not targets and re.search(r"(?<!\w)cc_library\(", text)
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
        if not found_any and not shim_only and build not in required_by_build:
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
