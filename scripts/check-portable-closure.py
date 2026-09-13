#!/usr/bin/env python3
"""Fail when a portable target can reach //src/platform.

This is the one portability rule the build graph does not enforce on its own.
Every platform package restricts its visibility to //src/platform, //src/ui,
//apps and //tests, so a portable target cannot depend on one -- until someone
widens that list, at which point Bazel is satisfied and only this check is
not. It reads the resolved transitive closure of every portable target, which
BUILD.bazel computes with a genquery from the registry in
bazel/portable_targets.bzl, and rejects any //src/platform label in it.

The rest of what this file used to assert is now a property of the graph:

- Qt is unreachable. @rules_qt is not in this module's repo mapping, the
  //bazel/qt:* aliases and the qt_compat / legacy shim packages are gated by
  //bazel/qt:qt_layer, and no portable package is in it. See
  docs/adr/0016-enforce-qt-reachability-by-visibility.md.
- A registered target that does not exist fails at analysis, because the
  genquery scope cannot resolve its label.
- JNI was never used anywhere in the repo.
"""

import sys
from pathlib import Path

# Bazel's py_test runner invokes this file from inside its own runfiles tree,
# and the genquery output has no canonical source-tree copy -- it exists only
# under bazel-out -- so reach it through the *unresolved* runfiles path.
GENQUERY_OUTPUT = Path(__file__).parent.parent / "portable_backend_closure"

PLATFORM_LABEL_PREFIX = "//src/platform/"


def main() -> int:
    if not GENQUERY_OUTPUT.is_file():
        print(
            "FAIL: portable_backend_closure genquery output not found at "
            f"{GENQUERY_OUTPUT} -- check the `data` entry for "
            "':portable_backend_closure' on the //:portable_closure target"
        )
        return 1

    labels = [line for line in GENQUERY_OUTPUT.read_text().splitlines() if line.strip()]
    violations = [label for label in labels if label.startswith(PLATFORM_LABEL_PREFIX)]
    if violations:
        print("FAIL: a portable target reaches platform code:")
        for label in violations:
            print(f"  {label}")
        print("\nPortable targets must not depend on //src/platform, directly or")
        print("through several layers of `deps`. Move the platform-facing code to")
        print("src/platform or src/ui, or reach it through a backend port.")
        return 1

    print(f"OK: no //src/platform label among the {len(labels)} labels in the portable closure.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
