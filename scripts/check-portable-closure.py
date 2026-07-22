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
}

FORBIDDEN = (
    re.compile(r'"@rules_qt//'),
    re.compile(r"QT_DEPS"),
    re.compile(r':qt_compat"'),
    re.compile(r'"@bazel_tools//tools/jdk:jni"'),
)


def portable_targets(text):
    """Yield (name, body) for every cc_library whose name lacks a qt_compat suffix."""
    for m in re.finditer(r'cc_library\(\s*name = "(?P<name>[^"]+)",(?P<body>.*?)\n\)', text, re.S):
        if m.group("name").endswith("qt_compat"):
            continue
        yield m.group("name"), m.group("body")


def main():
    build_files = []
    required_by_build = {}
    for root, required_targets in PORTABLE_ROOTS.items():
        found = sorted(root.rglob("BUILD.bazel"))
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
