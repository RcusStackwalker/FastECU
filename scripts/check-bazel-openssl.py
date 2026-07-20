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
    workflow = (ROOT / ".github/workflows/pr.yml").read_text()

    openssl_use_repo = re.search(
        r"use_repo\(\s*openssl,(?P<body>.*?)\)",
        module,
        flags=re.DOTALL,
    )
    if not openssl_use_repo or '"openssl"' not in openssl_use_repo.group("body"):
        errors.append("MODULE.bazel must expose the unified openssl repository")

    crypto = (ROOT / "src/algorithms/crypto/BUILD.bazel").read_text()
    cc_lib = re.search(
        r'cc_library\(\s*name = "crypto",(?P<body>.*?)\n\)',
        crypto,
        flags=re.DOTALL,
    )
    if not cc_lib or not re.search(r'"@openssl(?://:openssl)?"', cc_lib.group("body")):
        errors.append("//src/algorithms/crypto deps must include @openssl//:openssl")

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
