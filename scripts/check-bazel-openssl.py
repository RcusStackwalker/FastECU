#!/usr/bin/env python3
"""Check that Bazel's OpenSSL wiring matches the unified `openssl` repo."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    errors: list[str] = []

    module = (ROOT / "MODULE.bazel").read_text()
    build = (ROOT / "BUILD.bazel").read_text()
    workflow = (ROOT / ".github/workflows/pr.yml").read_text()

    openssl_use_repo = re.search(
        r"use_repo\(\s*openssl,(?P<body>.*?)\)",
        module,
        flags=re.DOTALL,
    )
    if not openssl_use_repo or '"openssl"' not in openssl_use_repo.group("body"):
        errors.append("MODULE.bazel must expose the unified openssl repository")

    core_common = re.search(
        r'qt_cc_library\(\s*name = "fastecu_core_common",(?P<body>.*?)\n\)',
        build,
        flags=re.DOTALL,
    )
    if not core_common or '"@openssl//:openssl"' not in core_common.group("body"):
        errors.append("//:fastecu_core_common deps must include @openssl//:openssl")

    bazel_prereq = re.search(
        r"name: Install Bazel prerequisites \(Windows\)(?P<body>.*?)(?:\n\s+- name:|\Z)",
        workflow,
        flags=re.DOTALL,
    )
    if not bazel_prereq or "OPENSSL_ROOT=" not in bazel_prereq.group("body"):
        errors.append("Windows Bazel CI prerequisites must export OPENSSL_ROOT")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print("Bazel OpenSSL wiring is configured.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
