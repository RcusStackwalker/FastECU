#!/usr/bin/env python3
"""Fail when a portable algorithms target declares a Qt dependency.

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
ALGORITHMS = ROOT / "src/algorithms"

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
    build_files = sorted(ALGORITHMS.rglob("BUILD.bazel"))
    if not build_files:
        print(f"FAIL: no BUILD.bazel files found under {ALGORITHMS}")
        return 1

    errors = []
    checked = 0
    for build in build_files:
        text = build.read_text()
        rel = build.relative_to(ROOT)
        found_any = False
        for name, body in portable_targets(text):
            found_any = True
            checked += 1
            for pattern in FORBIDDEN:
                if pattern.search(body):
                    errors.append(f"  {rel}: portable target '{name}' matches {pattern.pattern}")
        if not found_any:
            errors.append(f"  {rel}: no portable cc_library found")

    if errors:
        print("FAIL: Qt reached a portable algorithms target:")
        print("\n".join(errors))
        print("\nPortable targets must not declare QT_DEPS, @rules_qt//..., a")
        print(":qt_compat dep, or JNI. Put Qt overloads in the sibling")
        print(":qt_compat target and have the consumer depend on that instead.")
        return 1

    print(f"OK: {checked} portable algorithms targets, none reach Qt.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
