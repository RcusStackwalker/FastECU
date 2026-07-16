#!/usr/bin/env python3
"""Check that workflow Qt versions follow MODULE.bazel."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def module_qt_versions(text: str) -> set[str]:
    return set(re.findall(r'qt\.install\(\s*.*?version\s*=\s*"([^"]+)"', text, re.DOTALL))


def workflow_qt_version(text: str, path: str) -> str | None:
    match = re.search(r"^QT_VERSION:\s*['\"]([^'\"]+)['\"]\s*$", text, re.MULTILINE)
    if match:
        return match.group(1)
    print(f"{path} must define a top-level QT_VERSION", file=sys.stderr)
    return None


def main() -> int:
    errors: list[str] = []
    module_versions = module_qt_versions((ROOT / "MODULE.bazel").read_text())
    if len(module_versions) != 1:
        errors.append(f"MODULE.bazel must use exactly one Qt version, found: {sorted(module_versions)}")
        expected = None
    else:
        expected = next(iter(module_versions))

    for rel in (".github/workflows/pr.yml", ".github/workflows/release.yml"):
        actual = workflow_qt_version((ROOT / rel).read_text(), rel)
        if actual is None:
            errors.append(f"{rel} must define a top-level QT_VERSION")
        elif expected and actual != expected:
            errors.append(f"{rel} QT_VERSION is {actual}, expected {expected} from MODULE.bazel")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Qt version sync is configured for {expected}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
