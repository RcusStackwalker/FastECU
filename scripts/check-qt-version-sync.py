#!/usr/bin/env python3
"""Check that workflow Qt versions follow MODULE.bazel."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def module_qt_versions(text: str) -> tuple[set[str], list[str]]:
    versions: set[str] = set()
    errors: list[str] = []
    installs = list(re.finditer(r"qt\.install\((?P<body>.*?)\n\)", text, re.DOTALL))
    if not installs:
        errors.append("MODULE.bazel must define at least one qt.install(...) block")
        return versions, errors
    for index, install in enumerate(installs, start=1):
        matches = re.findall(r'^\s*version\s*=\s*"([^"]+)"\s*,\s*$', install.group("body"), re.MULTILINE)
        if len(matches) != 1:
            errors.append(f"MODULE.bazel qt.install block {index} must define exactly one literal version")
            continue
        versions.add(matches[0])
    return versions, errors


def workflow_qt_version(text: str, path: str) -> str | None:
    in_top_level_env = False
    for line in text.splitlines():
        if re.fullmatch(r"env:\s*", line):
            in_top_level_env = True
            continue
        if in_top_level_env:
            if line and not line[0].isspace():
                break
            match = re.fullmatch(r"  QT_VERSION:\s*['\"]([^'\"]+)['\"]\s*", line)
            if match:
                return match.group(1)
    return None


def main() -> int:
    errors: list[str] = []
    module_versions, module_errors = module_qt_versions((ROOT / "MODULE.bazel").read_text())
    errors.extend(module_errors)
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
