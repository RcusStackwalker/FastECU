#!/usr/bin/env python3
"""Freeze the serial_qt_compat visibility allowlist.

Every entry is a layering violation that step 5 (backend) or step 6 (ui)
removes. The list may shrink. It must never grow: a new entry means new
code took a dependency the modularization plan is trying to delete.
"""

import re
import sys

BUILD = "src/platform/desktop/common/serial/BUILD.bazel"

# Regenerate ONLY by removing entries. See the step 3 design doc.
FROZEN = {
    "//src/platform/desktop/common/serial:__pkg__",
    "//src/platform/desktop/common/transport:__pkg__",
    "//src/ui/desktop:__pkg__",
    "//src/ui/desktop/biu:__pkg__",
    "//tests:__pkg__",
}


def main():
    with open(BUILD) as f:
        text = f.read()
    # Anchor on the target, then scan only that rule's body. Pairing two lazy
    # `.*?` wildcards across the whole file backtracks superlinearly, and it
    # also fails open: if serial_qt_compat ever lost its visibility list, the
    # wildcard would run on and freeze some *later* target's list instead,
    # reporting OK while the real allowlist went unchecked. Bazel rules close
    # on a bare ")" at the start of a line, which bounds the body exactly.
    start = text.find('name = "serial_qt_compat"')
    if start < 0:
        print(f"FAIL: no serial_qt_compat target in {BUILD}")
        return 1
    end = text.find("\n)\n", start)
    body = text[start:end] if end >= 0 else text[start:]
    m = re.search(r"visibility = \[([^\]]*)\]", body)
    if not m:
        print(f"FAIL: no serial_qt_compat visibility list in {BUILD}")
        return 1
    actual = set(re.findall(r'"([^"]+)"', m.group(1)))
    added = actual - FROZEN
    if added:
        print("FAIL: serial_qt_compat allowlist grew. New entries:")
        for a in sorted(added):
            print(f"  {a}")
        print("\nThe allowlist may only shrink. If backend or UI code needs")
        print("serial_port_actions.h, that is the dependency steps 5 and 6")
        print("exist to remove -- do not add an entry here.")
        return 1
    removed = FROZEN - actual
    if removed:
        print("OK: allowlist shrank. Update FROZEN to match:")
        for r in sorted(removed):
            print(f"  removed {r}")
    print(f"OK: {len(actual)} entries, none added.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
