#!/usr/bin/env python3
"""Ratchet the per-family legacy flash drain (step 5 tail).

Every entry is a flash family that still speaks to SerialPortActions
directly instead of going through a portable FlashPlan + IFlashExecutor.
The set may shrink -- one entry per migrated family. It must never grow:
a new entry means a new legacy Qt flash operation was added instead of a
portable one.

Companion to scripts/check-serial-compat-allowlist.py, which freezes the
package-level visibility entry this drain eventually removes. That entry
cannot move until the last family lands, so it cannot show progress;
this script is what does. Both are deleted together in wave 7.

See docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LEGACY = ROOT / "src/platform/desktop/common/flash/legacy"
MARKER = "serial_port_actions.h"

# Not family migrations, so not part of the ratchet: these die with the
# package in wave 7, not one family at a time.
EXEMPT = {
    "legacy_flash_utils.cpp",
    "legacy_flash_utils_test.cpp",
}

# Regenerate ONLY by removing entries, one per migrated family.
REMAINING = {
    "bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.cpp",
    "bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp",
    "ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_1n83m_4m_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh7058_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh72531_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp",
    "ecu/flash_ecu_subaru_hitachi_sh7058_can_operation.cpp",
    "ecu/flash_ecu_subaru_hitachi_sh72543r_can_operation.cpp",
    "ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",
    "ecu/flash_ecu_subaru_unisia_jecs_operation.cpp",
    "jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp",
    "tcu/flash_tcu_subaru_denso_sh705x_can_operation.cpp",
    "tcu/flash_tcu_subaru_hitachi_m32r_can_operation.cpp",
    "tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.cpp",
}


def main():
    if not LEGACY.is_dir():
        # Wave 7 deleted the package: the drain is complete, and this
        # script should be deleted in the same commit.
        if REMAINING:
            print(f"FAIL: {LEGACY} is gone but REMAINING still lists {len(REMAINING)} families.")
            return 1
        print("OK: legacy flash package fully drained.")
        return 0

    actual = set()
    for path in sorted(LEGACY.rglob("*.cpp")):
        if path.name in EXEMPT:
            continue
        if MARKER in path.read_text(encoding="utf-8", errors="replace"):
            actual.add(path.relative_to(LEGACY).as_posix())

    added = actual - REMAINING
    if added:
        print("FAIL: legacy flash drain grew. New entries:")
        for a in sorted(added):
            print(f"  {a}")
        print("\nA new file here is a new Qt flash operation bound to")
        print("SerialPortActions. Write a portable FlashPlan +")
        print("IFlashExecutor under //src/backend/flash instead -- see")
        print("docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md.")
        return 1

    removed = REMAINING - actual
    if removed:
        print("OK: drain shrank. Update REMAINING to match:")
        for r in sorted(removed):
            print(f"  migrated {r}")
        return 1

    print(f"OK: {len(actual)} families remaining, none added.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
