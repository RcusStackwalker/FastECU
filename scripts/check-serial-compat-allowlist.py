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
#
# One entry is not debt: //src/platform/desktop/common/remote_utility is a
# same-layer sibling that depends on serial_qt_compat's websocketiodevice.h
# / qtrohelper.hpp (not serial_port_actions.h), a legitimate platform-internal
# edge that Step 1's serial_port_actions.h search does not surface. It is
# listed here so the freeze test passes, but it is not expected to shrink
# the way the serial_port_actions.h debt entries are.
FROZEN = {
    "//src/platform/desktop/common/remote_utility:__pkg__",
    "//src/backend/flash:__pkg__",
    "//src/backend/flash/bdm:__pkg__",
    "//src/backend/flash/bootmode:__pkg__",
    "//src/backend/flash/ecu:__pkg__",
    "//src/backend/flash/eeprom:__pkg__",
    "//src/backend/flash/jtag:__pkg__",
    "//src/backend/flash/tcu:__pkg__",
    "//src/backend/logging/protocols:__pkg__",
    "//src/platform/desktop/common/serial:__pkg__",
    "//src/platform/desktop/common/transport:__pkg__",
    "//src/ui/desktop:__pkg__",
    "//src/ui/desktop/biu:__pkg__",
    "//src/ui/desktop/flash/bdm:__pkg__",
    "//src/ui/desktop/flash/bootmode:__pkg__",
    "//src/ui/desktop/flash/ecu:__pkg__",
    "//src/ui/desktop/flash/eeprom:__pkg__",
    "//src/ui/desktop/flash/jtag:__pkg__",
    "//src/ui/desktop/flash/tcu:__pkg__",
    "//tests:__pkg__",
}


def main():
    with open(BUILD) as f:
        text = f.read()
    m = re.search(r'name = "serial_qt_compat".*?visibility = \[(.*?)\]', text, re.S)
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
