# Step 5 Tail — Wave 0 (Mitsu Colt M32R CAN) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port `FlashEcuMitsuM32rCanOperation` — the smallest ECU family and the only legacy flash class with existing wire-assertion tests — to a portable plan + executor, and install the `//:legacy_flash_drain` ratchet that measures every later wave.

**Architecture:** The legacy Qt `QThread` operation class is replaced by three pieces that already have a working reference in the step-5c EEPROM pair: a portable plan builder (`build_mitsu_colt_m32r_can_plan`) that validates everything decidable without I/O, a portable `IFlashExecutor` that owns the wire sequence and speaks only through `ICanFlashTransport`/`IClock`/`IEventSink`, and a Qt dialog that builds the plan, collects confirmations up front, and runs the executor on the existing `FlashWorker`. The legacy class is deleted in the same PR as the port, so the two can never diverge.

**Tech Stack:** C++23, Bazel 9.1.1, Qt 6.8.3, GoogleTest (`fastecu_portable_gtest` for Qt-free suites, `fastecu_qttest` for dialog suites), Python 3 for the guard scripts.

## Global Constraints

Copied verbatim from the [wave design](../specs/2026-08-08-step5-tail-flash-drain-design.md) and the [step-5 umbrella](../specs/2026-07-22-step5-backend-portable-design.md). Every task's requirements implicitly include this section.

- **Exceptions never cross a port.** Backend code converts to `Error` at the boundary. `void`-returning operations return `Result<void>` (`Status`).
- **The seven `ErrorKind` values are fixed:** `InvalidConfig`, `Timeout`, `Disconnected`, `BadResponse`, `Cancelled`, `Unsupported`, `Internal`. Do not add kinds.
- **Backend owns no threads, no filesystem, no widgets, no dialogs.** `src/backend/flash` carries no `QT_DEPS` — enforced package-wide by `//:portable_closure`'s `QT_FREE_PACKAGES`.
- **Pure logic uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView`.** `QByteArray` is a boundary type only, converted explicitly via `qt_bytes.h` (ADR 0004).
- **Every exchange in a portable executor carries a comment citing the legacy file and line it was transcribed from.** This is the only way a reviewer can check a port without a bench.
- **Deliberate divergences are named in the [flash qualification matrix](../../flash-qualification-matrix.md) `notes` column, never silent.** This wave has four; they are listed in Task 9.
- **Log message text is a compatibility contract.** Every `LOG_*` string reproduced in this plan is copied character-for-character from the legacy source and must not be reworded.
- **Every header needs `#pragma once`** (enforced by prek).
- **Prefer `std::string_view` by value** over `const char*`/`const std::string&` (ADR 0009); gmock matchers for property assertions (ADR 0010); `std::format` for message construction (ADR 0011).
- **Guard-script changes must be proven non-vacuous** — verified to fail when the target is absent as well as when it is non-conforming.
- **Commit directly to a branch off `master`, one commit per task.** A `no-commit-to-branch` prek hook forbids committing to `master` itself. Branch name: `feat/step5-tail-wave0-mitsu-colt-can`.

Gates, run at the end of every task that touches C++ or BUILD files:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `scripts/check-legacy-flash-drain.py` | Freezes the set of un-migrated legacy operation sources; may only shrink |
| `src/backend/flash/ecu/BUILD.bazel` | New portable package for ECU-family plans and executors |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_plan.{h,cpp}` | Preflight: MCU lookup, ROM-size and operation validation, plan assembly |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_plan_test.cpp` | Plan-builder suite (portable gtest) |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.{h,cpp}` | The wire sequence: connect, read, write, top-region bootstrap |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp` | Executor suite with byte-exact `expectWrite` scripts (portable gtest) |
| `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can_dialog_test.cpp` | Dialog orchestration suite (`fastecu_qttest`) |

**Modified:**

| File | Change |
|---|---|
| `BUILD.bazel` | `portable_backend_closure` genquery gains the two new targets; new `//:legacy_flash_drain` py_test |
| `scripts/check-portable-closure.py` | `PORTABLE_ROOTS` gains `src/backend/flash/ecu` |
| `src/backend/flash/flash_types.h` | New `FlashFamily` value, `MitsuColtM32rCanPlan`, two `ConfirmationSpec::Id` values, `FamilyPlan` alternative |
| `src/backend/flash/flash_plan.h` | `kernel` becomes `std::optional<KernelImage>`; `experimental_family_id()` gains a case |
| `src/backend/flash/flash_plan.cpp` | New family case |
| `src/backend/flash/flash_validation.cpp` | Optional-kernel rule, relaxed confirmation minimum, new variant/transport pairing |
| `src/backend/flash/eeprom/eeprom_read_plan.cpp` | Wraps its `KernelImage` in the new optional |
| `src/backend/flash/eeprom/denso_sh705x_eeprom_{kline,can}_executor.cpp` | Dereference the optional kernel |
| `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.{h,cpp}` | Rewritten onto plan + executor + `FlashWorker` |
| `src/ui/desktop/flash/ecu/BUILD.bazel` | New deps for the rewritten dialog; new dialog test target |
| `src/algorithms/protocol/mut_dma/BUILD.bazel` | Delete the dead `qt_compat` target |
| `src/algorithms/protocol/colt/BUILD.bazel` | Delete `qt_compat` (its last two callers go) |
| `docs/protocol-generalization-opportunities.md` | Correct the stale `src/backend/flash/flash_utils.*` reference |
| `docs/flash-qualification-matrix.md` | `FlashEcuMitsuM32rCan` row flipped, four divergences recorded |
| `docs/modularization-plan.md` | Wave 0 recorded under step 5 |

**Deleted:**

| File | Reason |
|---|---|
| `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.{h,cpp}` | Ported |
| `src/platform/desktop/common/flash/legacy/flash_ecu_mitsu_m32r_can_operation_test.cpp` | Its assertions move into the new executor/plan/dialog suites |
| `src/algorithms/protocol/colt/qt_colt.h` | Last callers deleted |
| `src/algorithms/protocol/mut_dma/qt_mut_dma.h` | Already had zero callers |

---

### Task 1: Install the `//:legacy_flash_drain` ratchet

The `serial_qt_compat` allowlist entry is package-level and cannot record per-family progress. This guard does, and it lands first — before any port — so every later task has something to shrink.

**Files:**
- Create: `scripts/check-legacy-flash-drain.py`
- Modify: `BUILD.bazel`

**Interfaces:**
- Consumes: nothing.
- Produces: `//:legacy_flash_drain` — a `py_test` that fails when `src/platform/desktop/common/flash/legacy/**` contains a file including `serial_port_actions.h` that is not listed in the script's `REMAINING` set. Later tasks shrink `REMAINING`.

- [ ] **Step 1: Write the guard script**

Create `scripts/check-legacy-flash-drain.py`:

```python
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
    "ecu/flash_ecu_mitsu_m32r_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_1n83m_4m_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh7058_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh72531_can_operation.cpp",
    "ecu/flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp",
    "ecu/flash_ecu_subaru_hitachi_m32r_can_operation.cpp",
    "ecu/flash_ecu_subaru_hitachi_m32r_kline_operation.cpp",
    "ecu/flash_ecu_subaru_hitachi_sh7058_can_operation.cpp",
    "ecu/flash_ecu_subaru_hitachi_sh72543r_can_operation.cpp",
    "ecu/flash_ecu_subaru_mitsu_m32r_kline_operation.cpp",
    "ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",
    "ecu/flash_ecu_subaru_unisia_jecs_operation.cpp",
    "jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp",
    "tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.cpp",
    "tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.cpp",
    "tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.cpp",
    "tcu/flash_tcu_subaru_denso_sh705x_can_operation.cpp",
    "tcu/flash_tcu_subaru_hitachi_m32r_can_operation.cpp",
    "tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.cpp",
}


def main():
    if not LEGACY.is_dir():
        # Wave 7 deleted the package: the drain is complete, and this
        # script should be deleted in the same commit.
        if REMAINING:
            print(f"FAIL: {LEGACY} is gone but REMAINING still lists "
                  f"{len(REMAINING)} families.")
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
        print("docs/superpowers/specs/"
              "2026-08-08-step5-tail-flash-drain-design.md.")
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
```

- [ ] **Step 2: Wire the Bazel target**

In `BUILD.bazel`, next to the existing `serial_compat_allowlist` target, add:

```python
py_test(
    name = "legacy_flash_drain",
    size = "small",
    srcs = ["scripts/check-legacy-flash-drain.py"],
    data = glob(["src/platform/desktop/common/flash/legacy/**/*.cpp"]),
    main = "scripts/check-legacy-flash-drain.py",
)
```

- [ ] **Step 3: Run the guard and verify it passes on a clean tree**

Run: `bazel test --config=release //:legacy_flash_drain --test_output=all`
Expected: PASS, with `OK: 27 families remaining, none added.`

If the count is not 27, the `REMAINING` set does not match the tree — reconcile it against the printed diff before continuing. Do not adjust the count to whatever the script prints without checking which file differs.

- [ ] **Step 4: Prove the guard non-vacuous — growth**

```bash
cp src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.cpp \
   src/platform/desktop/common/flash/legacy/ecu/zz_probe_operation.cpp
bazel test --config=release //:legacy_flash_drain --test_output=all; echo "exit=$?"
rm src/platform/desktop/common/flash/legacy/ecu/zz_probe_operation.cpp
```

Expected: FAIL, naming `ecu/zz_probe_operation.cpp` under "legacy flash drain grew".

- [ ] **Step 5: Prove the guard non-vacuous — absence**

```bash
git mv src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_operation.cpp /tmp/probe.cpp
bazel test --config=release //:legacy_flash_drain --test_output=all; echo "exit=$?"
git mv /tmp/probe.cpp src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_operation.cpp
```

Expected: FAIL, naming `migrated ecu/flash_ecu_subaru_unisia_jecs_operation.cpp`. This is the branch every later task relies on to force a `REMAINING` edit.

- [ ] **Step 6: Verify the tree is clean and the guard passes again**

Run: `git status --porcelain && bazel test --config=release //:legacy_flash_drain`
Expected: no untracked or modified files beyond the two intended ones; PASS.

- [ ] **Step 7: Commit**

```bash
git checkout -b feat/step5-tail-wave0-mitsu-colt-can
git add scripts/check-legacy-flash-drain.py BUILD.bazel
git commit -m "build: ratchet the per-family legacy flash drain

Adds //:legacy_flash_drain, freezing the 27 legacy flash operation
sources that still bind SerialPortActions directly. The set may only
shrink, one entry per migrated family.

The serial_qt_compat allowlist entry this eventually removes is
package-level and cannot move until the last family lands, so it
cannot show per-family progress. This guard does. Both are deleted
together in wave 7.

Proven non-vacuous in both directions: fails on an added file, and
fails on a removed one until REMAINING is updated to match."
```

---

### Task 2: Delete the dead `mut_dma:qt_compat` target

`qt_mut_dma.h` has zero callers outside `src/algorithms` — verified by searching `src`, `apps`, and `tests`. It is pure dead weight and its removal is independent of every other task.

**Files:**
- Delete: `src/algorithms/protocol/mut_dma/qt_mut_dma.h`
- Modify: `src/algorithms/protocol/mut_dma/BUILD.bazel`, `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing. This is a pure deletion.

- [ ] **Step 1: Confirm the target is genuinely unreferenced**

```bash
grep -rn "qt_mut_dma" src apps tests | grep -v "^src/algorithms/protocol/mut_dma/"
grep -rn "mut_dma:qt_compat" src apps tests BUILD.bazel
```

Expected: the first command prints nothing. The second prints exactly one line, in `src/ui/desktop/BUILD.bazel` — a `deps` entry with no corresponding `#include` anywhere, which is why it can go.

- [ ] **Step 2: Delete the header and both references**

```bash
git rm src/algorithms/protocol/mut_dma/qt_mut_dma.h
```

In `src/algorithms/protocol/mut_dma/BUILD.bazel`, delete the whole `qt_cc_library(name = "qt_compat", ...)` block. In `src/ui/desktop/BUILD.bazel`, delete the `"//src/algorithms/protocol/mut_dma:qt_compat",` line from `deps`.

- [ ] **Step 3: Verify the graph still builds and no target dangles**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel query 'rdeps(//..., //src/algorithms/protocol/mut_dma:qt_compat)' 2>&1 | tail -3
```

Expected: the build succeeds; the query errors with "target ... not declared in package", confirming nothing references it.

- [ ] **Step 4: Run the full test suite**

Run: `bazel test -k --config=release //... `
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add -A src/algorithms/protocol/mut_dma src/ui/desktop/BUILD.bazel
git commit -m "refactor: delete the dead mut_dma qt_compat shim

qt_mut_dma.h had zero callers outside src/algorithms. The one
remaining reference was a src/ui/desktop deps entry with no matching
include. First of the six shim retirements the step 5 tail carries;
the rest die with the families that use them."
```

---

### Task 3: Correct the stale reference in the protocol generalization notes

`docs/protocol-generalization-opportunities.md` lists `src/backend/flash/flash_utils.*` under "Consolidated foundations". Step 5e deleted that file. A reader following the tail's port-then-factor rule will consult this document, so it must not point at a file that no longer exists.

**Files:**
- Modify: `docs/protocol-generalization-opportunities.md`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing.

- [ ] **Step 1: Replace the stale bullet**

In the "Consolidated foundations" list, replace:

```markdown
- `src/backend/flash/flash_utils.*` owns common byte stuffing and ISO-15765 flash setup.
```

with:

```markdown
- `src/platform/desktop/common/flash/legacy/legacy_flash_utils.*` owns
  ISO-15765 flash setup and the `QString`-typed flash-device lookup shim, both
  transitional; `src/algorithms/checksum` owns `cks_add8`. Step 5e split the
  former `src/backend/flash/flash_utils.*` between them and deleted it.
```

- [ ] **Step 2: Record what unlocks sharing**

Append to the "Where not to generalize" section, after the existing "The intended boundary is:" list:

```markdown
The step 5 tail implements this condition as an ordering rule rather than a
judgment call: within a clone cluster, each family is ported to a tested
portable executor first, and only then is what is provably identical between
the tested executors factored into a shared core. Extraction never happens
against the untested Qt sources. See the
[tail design](superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md).
```

- [ ] **Step 3: Verify links resolve**

Run: `prek run --files docs/protocol-generalization-opportunities.md`
Expected: `lychee` passes. (`don't commit to branch` also runs; it passes on a feature branch.)

- [ ] **Step 4: Commit**

```bash
git add docs/protocol-generalization-opportunities.md
git commit -m "docs: correct the stale flash_utils reference

Step 5e deleted src/backend/flash/flash_utils.*, splitting it between
the platform legacy package and src/algorithms/checksum. Also records
that the tail's port-then-factor ordering is what implements this
document's own precondition for sharing."
```

---

### Task 4: Extend the plan model for a non-kernel family

`validate_and_build` currently encodes two EEPROM-shaped assumptions that reject any Colt CAN plan outright: it requires a non-empty `KernelImage`, and it requires at least one confirmation. The Colt family uploads no kernel — it drives the ECU's own vendor bootloader, and its RAM helper routines are compile-time constants in `mitsu_colt_can_protocol.h`, not a loaded file — and its read path prompts for nothing.

**Files:**
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/flash_plan.h`, `src/backend/flash/flash_plan.cpp`
- Modify: `src/backend/flash/flash_validation.cpp`
- Modify: `src/backend/flash/eeprom/eeprom_read_plan.cpp`
- Modify: `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp`, `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp`
- Test: `src/backend/flash/flash_validation_test.cpp`, `src/backend/flash/flash_types_test.cpp`

**Interfaces:**
- Consumes: `FlashPlanFields`, `validate_and_build`, `FlashPlan` from Task 0 (existing code).
- Produces:
  - `FlashFamily::MitsuColtM32rCan`
  - `struct MitsuColtM32rCanPlan { std::uint32_t request_id; std::uint32_t response_id; int bitrate; bool extended_id; bool use_vendor_challenge; bytes::Byte session_id; }`
  - `ConfirmationSpec::Id::EraseTrigger`, `ConfirmationSpec::Id::TopRegionBootstrap`
  - `FlashPlanFields::kernel` and `FlashPlan::kernel()` are now `std::optional<KernelImage>`

- [ ] **Step 1: Write the failing validation tests**

Append to `src/backend/flash/flash_validation_test.cpp`:

```cpp
namespace
{

// Minimal fields for a family that uploads no kernel and prompts for
// nothing -- the shape the Mitsu Colt CAN read plan produces.
fastecu::flash::FlashPlanFields kernellessReadFields()
{
    using namespace fastecu::flash;
    FlashPlanFields fields;
    fields.operation = FlashOperation::Read;
    fields.family = FlashFamily::MitsuColtM32rCan;
    fields.transport = TransportKind::CanIso15765;
    fields.target_id = "mitsu_ecu_m32r_can";
    fields.mcu_name = "M32R_384KB_1block";
    fields.transfer_region = MemoryRegion{0x00008000, 0x00058000};
    fields.kernel = std::nullopt;
    fields.family_plan = MitsuColtM32rCanPlan{0x7e0, 0x7e8, 500000, false, false, 0x81};
    return fields;
}

} // namespace

TEST(FlashValidation, AcceptsAPlanWithNoKernelAndNoConfirmations)
{
    const auto plan = fastecu::flash::validate_and_build(kernellessReadFields());

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_TRUE(plan->confirmations().empty());
    EXPECT_EQ(plan->experimental_family_id(), "MitsuColtM32rCan");
}

TEST(FlashValidation, RejectsAPresentKernelWithNoBytes)
{
    auto fields = kernellessReadFields();
    fields.kernel = fastecu::flash::KernelImage{"colt", 0x800000, {}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("kernel bytes"));
}

TEST(FlashValidation, RejectsAPresentKernelWithNoId)
{
    auto fields = kernellessReadFields();
    fields.kernel = fastecu::flash::KernelImage{"", 0x800000, {0x01, 0x02}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("kernel id"));
}

TEST(FlashValidation, RejectsAColtPlanOnAKlineTransport)
{
    auto fields = kernellessReadFields();
    fields.transport = fastecu::flash::TransportKind::Kline;

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("does not match transport kind"));
}

TEST(FlashValidation, StillRejectsDuplicateConfirmationIds)
{
    using fastecu::flash::ConfirmationSpec;
    auto fields = kernellessReadFields();
    fields.operation = fastecu::flash::FlashOperation::Write;
    fields.image = bytes::Bytes(0x80000, 0x00);
    fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::EraseTrigger, {}},
                            ConfirmationSpec{ConfirmationSpec::Id::EraseTrigger, {}}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("duplicate confirmation id"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/flash:flash_validation_test --test_output=all`
Expected: FAIL to compile — `MitsuColtM32rCan` is not a member of `FlashFamily`, `MitsuColtM32rCanPlan` is undeclared, and `fields.kernel = std::nullopt` does not convert.

- [ ] **Step 3: Extend `flash_types.h`**

Add `MitsuColtM32rCan` to `FlashFamily`, immediately after the two EEPROM values:

```cpp
enum class FlashFamily
{
    DensoSh705xEepromKline,
    DensoSh705xEepromCan,
    // Step 5 tail, wave 0. Serves both mitsu_ecu_m32r_can and
    // mitsu_ecu_m32r_can_vendor_ext; the vendor challenge is a plan flag,
    // not a separate family, matching the legacy class it replaces.
    MitsuColtM32rCan,
};
```

Add the two confirmation ids to `ConfirmationSpec::Id`:

```cpp
    enum class Id
    {
        BeginEepromRead,
        InspectEepromBytes,
        CycleIgnition,
        // Step 5 tail, wave 0. Both are collected by the desktop dialog
        // BEFORE the executor starts: a synchronous, dialog-free executor
        // cannot block mid-run for a human answer. Presence in
        // FlashPlan::confirmations() therefore means "granted" -- an
        // operator who declines either one causes the dialog to never build
        // a plan at all.
        EraseTrigger,
        TopRegionBootstrap,
    };
```

Add the family plan struct next to the EEPROM ones, and extend the variant:

```cpp
struct MitsuColtM32rCanPlan
{
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false -- build_request() hardcodes the
                               // 11-bit physical request id
    bool use_vendor_challenge; // mitsu_ecu_m32r_can_vendor_ext only
    bytes::Byte session_id;    // kSessionBasic (0x81) for Read,
                               // kSessionBootload (0x85) for Write
};

using FamilyPlan = std::variant<
    DensoSh705xEepromKlinePlan,
    DensoSh705xEepromCanPlan,
    MitsuColtM32rCanPlan>;
```

- [ ] **Step 4: Make the kernel optional in `flash_plan.h`**

Add `#include <optional>` if absent, then change the field and its accessor:

```cpp
    // Optional because not every family uploads one. The EEPROM pair loads a
    // kernel file and uploads it; the Mitsu Colt CAN family drives the ECU's
    // own vendor bootloader and uploads only compile-time RAM helper routines
    // that are protocol constants, not a loaded image.
    std::optional<KernelImage> kernel;
```

```cpp
    const std::optional<KernelImage>& kernel() const
    {
        return fields_.kernel;
    }
```

Add the new family case to `flash_plan.cpp`:

```cpp
    case FlashFamily::MitsuColtM32rCan:
        return "MitsuColtM32rCan";
```

- [ ] **Step 5: Relax the two rules in `flash_validation.cpp`**

Replace the unconditional kernel block:

```cpp
    if (fields.kernel.id.empty())
    {
        return fail(ErrorKind::InvalidConfig, "kernel id must not be empty");
    }
    if (fields.kernel.bytes.empty())
    {
        return fail(ErrorKind::InvalidConfig, "kernel bytes must not be empty");
    }
    const std::uint64_t kernel_end =
        static_cast<std::uint64_t>(fields.kernel.load_address) + fields.kernel.bytes.size();
    if (kernel_end > static_cast<std::uint64_t>(0xffffffffu))
    {
        return fail(ErrorKind::InvalidConfig, "kernel upload range overflows a 32-bit address space");
    }
```

with a presence-conditional form. A declared kernel is still fully validated; an absent one is now legal:

```cpp
    if (fields.kernel.has_value())
    {
        if (fields.kernel->id.empty())
        {
            return fail(ErrorKind::InvalidConfig, "kernel id must not be empty");
        }
        if (fields.kernel->bytes.empty())
        {
            return fail(ErrorKind::InvalidConfig, "kernel bytes must not be empty");
        }
        const std::uint64_t kernel_end =
            static_cast<std::uint64_t>(fields.kernel->load_address) + fields.kernel->bytes.size();
        if (kernel_end > static_cast<std::uint64_t>(0xffffffffu))
        {
            return fail(ErrorKind::InvalidConfig,
                        "kernel upload range overflows a 32-bit address space");
        }
    }
```

Delete the confirmation minimum entirely — the duplicate check below it stays:

```cpp
    if (fields.confirmations.empty())
    {
        return fail(ErrorKind::InvalidConfig, "at least one confirmation must be declared");
    }
```

Extend `family_matches_transport_variant` so the CAN case accepts either CAN family:

```cpp
bool family_matches_transport_variant(const FlashPlanFields& fields)
{
    switch (fields.transport)
    {
    case TransportKind::Kline:
        return std::holds_alternative<DensoSh705xEepromKlinePlan>(fields.family_plan);
    case TransportKind::CanIso15765:
        return std::holds_alternative<DensoSh705xEepromCanPlan>(fields.family_plan) ||
               std::holds_alternative<MitsuColtM32rCanPlan>(fields.family_plan);
    }
    return false;
}
```

- [ ] **Step 6: Update the three EEPROM call sites**

In `src/backend/flash/eeprom/eeprom_read_plan.cpp`, exactly one line changes. The designated initializer at line 164 (`.kernel = KernelImage{...}`) converts to the optional implicitly and stays as-is. Line 190 does not:

```cpp
    input.kernel.bytes = std::move(*kernel_bytes);
```

becomes

```cpp
    input.kernel->bytes = std::move(*kernel_bytes);
```

In both `denso_sh705x_eeprom_kline_executor.cpp` and `denso_sh705x_eeprom_can_executor.cpp`, every `plan.kernel()` use becomes `*plan.kernel()`. These executors already ran `check_family_transport_match` first and only ever run against plans their own builder produced, which always sets a kernel, so the dereference is sound — add this comment at the first such use in each file:

```cpp
    // Safe to dereference: build_eeprom_read_plan always sets a kernel, and
    // check_family_transport_match above rejects any plan not built by it.
```

- [ ] **Step 7: Run the tests to verify they pass**

```bash
bazel test --config=release //src/backend/flash:all //src/backend/flash/eeprom:all --test_output=errors
```

Expected: all PASS, including the five new `FlashValidation` cases and every pre-existing EEPROM suite unchanged.

- [ ] **Step 8: Verify no golden vector moved**

Run: `bazel test --config=release //src/backend/flash/eeprom:eeprom_read_plan_goldens_test --test_output=all`
Expected: PASS with no diff. This is the guard that the optional-kernel change was type-level only.

- [ ] **Step 9: Commit**

```bash
git add src/backend/flash
git commit -m "refactor(flash): allow families that upload no kernel

FlashPlanFields::kernel becomes std::optional<KernelImage>, and the
'at least one confirmation' rule is dropped. Both were EEPROM-shaped
assumptions that rejected any Mitsu Colt CAN plan: that family drives
the ECU's own vendor bootloader and its RAM helper routines are
protocol constants, not a loaded image, and its read path prompts for
nothing.

A declared kernel is still validated exactly as before. Adds
FlashFamily::MitsuColtM32rCan, MitsuColtM32rCanPlan, and the
EraseTrigger/TopRegionBootstrap confirmation ids."
```

---

### Task 5: Portable plan builder

Everything the legacy `execute()` and `write_mem()` decide before touching the ECU moves here: MCU lookup, operation support, and the ROM-size floor.

**Files:**
- Create: `src/backend/flash/ecu/BUILD.bazel`
- Create: `src/backend/flash/ecu/mitsu_colt_m32r_can_plan.{h,cpp}`
- Test: `src/backend/flash/ecu/mitsu_colt_m32r_can_plan_test.cpp`
- Modify: `BUILD.bazel`, `scripts/check-portable-closure.py`

**Interfaces:**
- Consumes: `FlashFamily::MitsuColtM32rCan`, `MitsuColtM32rCanPlan`, `ConfirmationSpec::Id::{EraseTrigger,TopRegionBootstrap}`, optional `kernel` — all from Task 4. `fastecu::flash::find_flash_device_index` from `//src/backend/flash:flash_device_lookup`. `MitsuColtCan::k*` constants from `//src/algorithms/protocol/colt`.
- Produces:
  ```cpp
  Result<FlashPlan> build_mitsu_colt_m32r_can_plan(FlashOperation operation,
                                                   std::string_view protocol_name,
                                                   std::string_view mcu_type,
                                                   bool use_vendor_challenge,
                                                   std::optional<bytes::Bytes> image);
  ```

- [ ] **Step 1: Write the failing plan tests**

Create `src/backend/flash/ecu/mitsu_colt_m32r_can_plan_test.cpp`:

```cpp
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::flash::build_mitsu_colt_m32r_can_plan;
using fastecu::flash::ConfirmationSpec;
using fastecu::flash::FlashOperation;
using fastecu::flash::MitsuColtM32rCanPlan;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "mitsu_ecu_m32r_can";
constexpr std::string_view kMcu = "M32R_384KB_1block";

bytes::Bytes fullRom()
{
    return bytes::Bytes(MitsuColtCan::kTopRegionEnd, 0x00);
}

TEST(MitsuColtM32rCanPlan, ReadPlanCoversTheFirstFlashBlock)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu,
                                                     false, std::nullopt);

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    // Legacy: flashdevices[idx].fblocks[0].{start,len}, transcribed from
    // flash_ecu_mitsu_m32r_can_operation.cpp:44-45.
    EXPECT_EQ(plan->transfer_region().start, 0x00008000u);
    EXPECT_EQ(plan->transfer_region().length, 0x00058000u);
    EXPECT_TRUE(plan->erase_regions().empty());
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_TRUE(plan->confirmations().empty());
}

TEST(MitsuColtM32rCanPlan, ReadPlanSelectsTheBasicDiagnosticSession)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu,
                                                     false, std::nullopt);

    ASSERT_TRUE(plan.has_value());
    const auto& family = std::get<MitsuColtM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.session_id, MitsuColtCan::kSessionBasic);
    EXPECT_FALSE(family.use_vendor_challenge);
    EXPECT_EQ(family.request_id, 0x7e0u);
    EXPECT_EQ(family.response_id, 0x7e8u);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
}

TEST(MitsuColtM32rCanPlan, VendorExtensionProtocolSetsTheChallengeFlag)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(
        FlashOperation::Read, "mitsu_ecu_m32r_can_vendor_ext", kMcu, true, std::nullopt);

    ASSERT_TRUE(plan.has_value());
    EXPECT_TRUE(std::get<MitsuColtM32rCanPlan>(plan->family_plan()).use_vendor_challenge);
}

TEST(MitsuColtM32rCanPlan, WritePlanSelectsBootloadSessionAndDeclaresBothGates)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     false, fullRom());

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(std::get<MitsuColtM32rCanPlan>(plan->family_plan()).session_id,
              MitsuColtCan::kSessionBootload);
    EXPECT_THAT(plan->confirmations(),
                testing::UnorderedElementsAre(
                    testing::Field(&ConfirmationSpec::id, ConfirmationSpec::Id::EraseTrigger),
                    testing::Field(&ConfirmationSpec::id,
                                   ConfirmationSpec::Id::TopRegionBootstrap)));
}

TEST(MitsuColtM32rCanPlan, WritePlanTransfersTheUserspaceRange)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     false, fullRom());

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->transfer_region().start, MitsuColtCan::kUserspaceStart);
    EXPECT_EQ(plan->transfer_region().length,
              MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart);
}

TEST(MitsuColtM32rCanPlan, RejectsAnUnknownMcuType)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kProtocol,
                                                     "NOT_A_REAL_MCU", false, std::nullopt);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:27.
    EXPECT_THAT(plan.error().detail, HasSubstr("Unknown MCU type: NOT_A_REAL_MCU"));
}

TEST(MitsuColtM32rCanPlan, RejectsARomShorterThanTheTopRegionEnd)
{
    bytes::Bytes shortRom(MitsuColtCan::kTopRegionEnd - 1, 0x00);

    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     false, std::move(shortRom));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:400.
    EXPECT_THAT(plan.error().detail, HasSubstr("ROM file too small"));
}

TEST(MitsuColtM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::TestWrite, kProtocol,
                                                     kMcu, false, fullRom());

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
    EXPECT_THAT(plan.error().detail, HasSubstr("test_write"));
}

TEST(MitsuColtM32rCanPlan, RejectsAWriteWithNoImage)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     false, std::nullopt);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

} // namespace
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/backend/flash/ecu:mitsu_colt_m32r_can_plan_test`
Expected: FAIL — no such package `src/backend/flash/ecu`.

- [ ] **Step 3: Write the header**

Create `src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h`:

```cpp
#pragma once
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Builds a Mitsubishi Colt CZT (Z37A, ROM 47110032) M32R CAN plan. Portable
// equivalent of the preflight half of FlashEcuMitsuM32rCanOperation, deleted
// by this wave.
//
// Owns no state and reads no files: unlike the EEPROM pair, this family
// loads no kernel and consults no catalog. Everything it needs is the
// selected protocol name, the MCU type string from the ROM definition, the
// vendor-challenge flag MainWindow already passes to the dialog, and (for a
// write) the ROM image.
//
// Three checks the legacy class performed AFTER opening the port and
// completing the bootloader handshake run here instead, before any I/O:
// unknown MCU type, ROM shorter than kTopRegionEnd, and an unsupported
// operation. That ordering change is deliberate -- it is the FlashPlan
// contract, and it means a misconfigured write never reaches the ECU -- and
// is recorded in the flash qualification matrix.
//
// TestWrite is rejected as Unsupported. protocols.cfg declares
// test_write=no for both mitsu_ecu_m32r_can and mitsu_ecu_m32r_can_vendor_ext,
// while the legacy class silently returned success after performing only the
// diagnostic-session handshake. Returning Unsupported follows the step-5c
// precedent set for the EEPROM write gap rather than legitimizing a no-op.
Result<FlashPlan> build_mitsu_colt_m32r_can_plan(FlashOperation operation,
                                                 std::string_view protocol_name,
                                                 std::string_view mcu_type,
                                                 bool use_vendor_challenge,
                                                 std::optional<bytes::Bytes> image);

} // namespace fastecu::flash
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/flash/ecu/mitsu_colt_m32r_can_plan.cpp`:

```cpp
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <format>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{

Result<FlashPlan> build_mitsu_colt_m32r_can_plan(FlashOperation operation,
                                                 std::string_view protocol_name,
                                                 std::string_view mcu_type,
                                                 bool use_vendor_challenge,
                                                 std::optional<bytes::Bytes> image)
{
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by this family; protocols.cfg declares "
                    "test_write=no and the legacy implementation performed only a "
                    "diagnostic-session handshake");
    }

    // Legacy: flash_ecu_mitsu_m32r_can_operation.cpp:24-29.
    const int mcu_index = find_flash_device_index(mcu_type);
    if (mcu_index < 0)
    {
        return fail(ErrorKind::InvalidConfig, std::format("Unknown MCU type: {}", mcu_type));
    }

    FlashPlanFields fields;
    fields.operation = operation;
    fields.family = FlashFamily::MitsuColtM32rCan;
    fields.transport = TransportKind::CanIso15765;
    fields.target_id = std::string(protocol_name);
    fields.mcu_name = std::string(mcu_type);
    fields.kernel = std::nullopt;

    if (operation == FlashOperation::Read)
    {
        // Legacy: flash_ecu_mitsu_m32r_can_operation.cpp:44-45.
        fields.transfer_region = MemoryRegion{flashdevices[mcu_index].fblocks[0].start,
                                              flashdevices[mcu_index].fblocks[0].len};
        fields.family_plan = MitsuColtM32rCanPlan{
            0x7e0, 0x7e8, 500000, false, use_vendor_challenge, MitsuColtCan::kSessionBasic};
        return validate_and_build(std::move(fields));
    }

    if (!image.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Write plans must carry a ROM image");
    }
    // Legacy: flash_ecu_mitsu_m32r_can_operation.cpp:398-402.
    if (image->size() < MitsuColtCan::kTopRegionEnd)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("ROM file too small: need at least 0x{:x} bytes",
                                MitsuColtCan::kTopRegionEnd));
    }

    // The declared transfer region is the userspace write range; the
    // top-region bootstrap reads and writes kTopRegionStart..kTopRegionEnd
    // conditionally and is not part of the declared transfer accounting,
    // exactly as the legacy progress reporting treated it.
    fields.transfer_region =
        MemoryRegion{MitsuColtCan::kUserspaceStart,
                     MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart};
    fields.image = std::move(image);
    fields.family_plan = MitsuColtM32rCanPlan{
        0x7e0, 0x7e8, 500000, false, use_vendor_challenge, MitsuColtCan::kSessionBootload};
    fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::EraseTrigger, {}},
                            ConfirmationSpec{ConfirmationSpec::Id::TopRegionBootstrap, {}}};

    return validate_and_build(std::move(fields));
}

} // namespace fastecu::flash
```

- [ ] **Step 5: Write the BUILD file**

Create `src/backend/flash/ecu/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")

package(default_visibility = [
    "//src/backend:__subpackages__",
    "//src/platform:__subpackages__",
    "//src/ui:__subpackages__",
    "//tests:__pkg__",
])

# Portable. No Qt, no platform, no filesystem, no thread.
cc_library(
    name = "mitsu_colt_m32r_can_plan",
    srcs = ["mitsu_colt_m32r_can_plan.cpp"],
    hdrs = ["mitsu_colt_m32r_can_plan.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/colt",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/flash:flash_plan",
        "//src/backend/flash:flash_types",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "mitsu_colt_m32r_can_plan_test",
    srcs = ["mitsu_colt_m32r_can_plan_test.cpp"],
    deps = [
        ":mitsu_colt_m32r_can_plan",
        "//src/algorithms/protocol/colt",
    ],
)
```

- [ ] **Step 6: Register the package in the portable closure**

In `scripts/check-portable-closure.py`, add to `PORTABLE_ROOTS` immediately after the `src/backend/flash/eeprom` entry:

```python
    ROOT / "src/backend/flash/ecu": {
        "mitsu_colt_m32r_can_plan",
    },
```

In `BUILD.bazel`, add to the `portable_backend_closure` genquery's target list, after the eeprom entries:

```python
        "//src/backend/flash/ecu:mitsu_colt_m32r_can_plan",
```

- [ ] **Step 7: Run the tests to verify they pass**

```bash
bazel test --config=release //src/backend/flash/ecu:all --test_output=errors
bazel test --config=release //:portable_closure --test_output=all
```

Expected: all plan tests PASS; `portable_closure` PASS with the new target counted and no Qt reached.

- [ ] **Step 8: Prove the closure registration non-vacuous**

```bash
# Temporarily give the portable target a Qt dep and confirm the guard fires.
sed -i.bak 's|"//src/backend/ports",|"//src/backend/ports",\n        "@rules_qt//:qt_core",|' \
    src/backend/flash/ecu/BUILD.bazel
bazel test --config=release //:portable_closure --test_output=all; echo "exit=$?"
mv src/backend/flash/ecu/BUILD.bazel.bak src/backend/flash/ecu/BUILD.bazel
```

Expected: FAIL naming `//src/backend/flash/ecu:mitsu_colt_m32r_can_plan` as reaching Qt. Then re-run `bazel test --config=release //:portable_closure` and confirm PASS after restore.

- [ ] **Step 9: Commit**

```bash
git add src/backend/flash/ecu BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(flash): portable Mitsu Colt M32R CAN plan builder

Preflight half of FlashEcuMitsuM32rCanOperation: MCU lookup, ROM-size
floor, operation support, and family plan assembly, with no I/O.

Three checks the legacy class ran after opening the port and completing
the handshake now run before any I/O, so a misconfigured write never
reaches the ECU. TestWrite is rejected as Unsupported, matching
protocols.cfg's test_write=no and following the step-5c EEPROM
precedent rather than legitimizing the legacy no-op success."
```

---

### Task 6: Portable executor — connect and read

The read path is the whole of `connect_bootloader()`, `readFlashRange()`, and `read_mem()`. Write lands in Task 7 so a reviewer can gate the two independently.

**Files:**
- Create: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.{h,cpp}`
- Test: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `BUILD.bazel`, `scripts/check-portable-closure.py`

**Interfaces:**
- Consumes: `build_mitsu_colt_m32r_can_plan` (Task 5); `IFlashExecutor`, `ICanFlashTransport`, `check_family_transport_match` from `//src/backend/flash:flash_executor`; `ScriptedCanFlashTransport` from `//src/backend/flash/testing:scripted_flash_transports`; `FakeClock` from `//src/backend/ports/testing:fake_clock`; `MitsuColtCan::*` and `MitsuColtCanVendorExt::*` builders from `//src/algorithms/protocol/colt`; `nrc_description` (global scope, not namespaced) from `//src/algorithms/diagnostics`; `SsmProtocol::toHex(bytes::ByteView)` from the **portable** `//src/algorithms/protocol/ssm` — it emits lowercase `"%02x "` per byte, trailing space included, and the log text depends on that exact form.
- Produces: `class MitsuColtM32rCanExecutor final : public IFlashExecutor` with the standard `execute(plan, transport, clock, cancellation, events)` signature.

- [ ] **Step 1: Write the failing executor tests**

Create `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`:

```cpp
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::flash::build_mitsu_colt_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::MitsuColtM32rCanExecutor;
using fastecu::flash::ScriptedCanFlashTransport;
using testing::Contains;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "mitsu_ecu_m32r_can";
// 128KB single block starting at userspace: keeps the scripted read short.
constexpr std::string_view kMcu = "M32R_128KB";

// Recording sink so the log-text compatibility contract is asserted, not
// assumed. Every string below is copied from the legacy source.
class RecordingSink final : public fastecu::IEventSink
{
  public:
    void log(LogLevel level, std::string_view message) override
    {
        lines.emplace_back(level, std::string(message));
        messages.emplace_back(message);
    }
    void progress(int done, int total) override
    {
        progressCalls.emplace_back(done, total);
    }
    void notice(std::string_view message) override
    {
        notices.emplace_back(message);
    }

    std::vector<std::pair<LogLevel, std::string>> lines;
    std::vector<std::string> messages;
    std::vector<std::pair<int, int>> progressCalls;
    std::vector<std::string> notices;
};

// Every request on this bus carries a 4-byte big-endian 0x7E0 prefix
// (legacy build_request, flash_ecu_mitsu_m32r_can_operation.cpp:58-64).
bytes::Bytes request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Responses carry the 4-byte 0x7E8 reply id; the legacy code indexes
// received.at(4) for the service byte throughout.
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e8);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

fastecu::flash::FlashPlan readPlan(bool vendor = false)
{
    auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, vendor,
                                               std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

TEST(MitsuColtM32rCanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    // A plan built for another family must be rejected by
    // check_family_transport_match before configure()/open() or any write --
    // the scripted transport is left completely untouched, which is the
    // assertion that matters here.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // Hand-built rather than produced by a builder: the point is a plan this
    // executor must refuse, and only validate_and_build can make a FlashPlan.
    fastecu::flash::FlashPlanFields fields;
    fields.operation = FlashOperation::Read;
    fields.family = fastecu::flash::FlashFamily::DensoSh705xEepromCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = "sub_ecu_denso_sh705x_eeprom_can";
    fields.mcu_name = "SH7058";
    fields.transfer_region = fastecu::flash::MemoryRegion{0x0, 0x100};
    fields.kernel = fastecu::flash::KernelImage{"k", 0xffff6004, {0x01, 0x02}};
    fields.family_plan = fastecu::flash::DensoSh705xEepromCanPlan{
        fastecu::flash::EepromReadMode::Mode2, fastecu::flash::DensoSecurityVariant::Stock,
        0x7e0, 0x7e8, 500000, false};
    auto foreign = fastecu::flash::validate_and_build(std::move(fields));
    ASSERT_TRUE(foreign.has_value()) << foreign.error().detail;

    const auto result =
        executor.execute(*foreign, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("does not match this executor"));
    EXPECT_TRUE(events.messages.empty());
}

TEST(MitsuColtM32rCanExecutor, ReadPerformsTheBasicHandshakeThenChunkedReads)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    // Legacy: connect_bootloader() sends SID 0x10 with kSessionBasic and
    // requires (0x10+0x40, 0x81) back (lines 120-129).
    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));

    // 128KB at kFlashReadBlockSize (192) per chunk.
    const std::uint32_t start = plan.transfer_region().start;
    const std::uint32_t length = plan.transfer_region().length;
    for (std::uint32_t addr = start; addr < start + length;
         addr += MitsuColtCan::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = start + length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < MitsuColtCan::kFlashReadBlockSize ? remaining
                                                          : MitsuColtCan::kFlashReadBlockSize);
        transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(addr, chunk)));
        bytes::Bytes reply = response({0x63});
        reply.insert(reply.end(), chunk, 0xAB);
        transport.queueRead(reply);
    }

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), length);
    EXPECT_THAT(*result->read_bytes, testing::Each(0xAB));
    EXPECT_THAT(events.messages, Contains(std::string("Diagnostic session ok")));
    EXPECT_THAT(events.messages, Contains(std::string("ROM read complete")));
}

TEST(MitsuColtM32rCanExecutor, ReadRejectsAWrongDiagnosticSessionResponse)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x7f, 0x10, 0x12})); // negative response

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:126.
    EXPECT_THAT(events.messages, Contains(HasSubstr("Wrong response from ECU: ")));
}

TEST(MitsuColtM32rCanExecutor, ReadReportsAnEmptyReplyAsTimeout)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queue_no_frame();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
}

TEST(MitsuColtM32rCanExecutor, ReadPropagatesADisconnectedTransport)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST(MitsuColtM32rCanExecutor, ReadStopsAtTheNextChunkWhenCancelled)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));
    cancellation.trip();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST(MitsuColtM32rCanExecutor, VendorChallengePrecedesTheDiagnosticSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(/*vendor=*/true);

    // Legacy ordering, flash_ecu_mitsu_m32r_can_operation.cpp:84-129:
    // seed request, key answer, then the diagnostic session.
    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    transport.queueRead(response({0x63, 0x27, 0x41, 0x12, 0x34, 0x56, 0x78}));

    const std::uint32_t key = MitsuColtCanVendorExt::challengeInverseTransform(0x12345678);
    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeKey(key)));
    transport.queueRead(response({0x63, 0x27, 0x42}));

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queue_no_frame(); // stop here: ordering is what this pins

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.messages, Contains(std::string("Vendor challenge accepted")));
}

TEST(MitsuColtM32rCanExecutor, VendorChallengeRejectionStopsBeforeTheSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(/*vendor=*/true);

    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    transport.queueRead(response({0x7f, 0x23, 0x33}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:93.
    EXPECT_THAT(events.messages,
                Contains(HasSubstr("Wrong vendor challenge response from ECU: ")));
}

TEST(MitsuColtM32rCanExecutor, ReadEmitsMonotonicProgress)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));

    const std::uint32_t start = plan.transfer_region().start;
    const std::uint32_t length = plan.transfer_region().length;
    for (std::uint32_t addr = start; addr < start + length;
         addr += MitsuColtCan::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = start + length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < MitsuColtCan::kFlashReadBlockSize ? remaining
                                                          : MitsuColtCan::kFlashReadBlockSize);
        transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(addr, chunk)));
        bytes::Bytes reply = response({0x63});
        reply.insert(reply.end(), chunk, 0x00);
        transport.queueRead(reply);
    }

    ASSERT_TRUE(executor.execute(plan, transport, clock, cancellation.token(), events)
                    .has_value());

    ASSERT_FALSE(events.progressCalls.empty());
    EXPECT_EQ(events.progressCalls.front().second, static_cast<int>(length));
    EXPECT_EQ(events.progressCalls.back().first, static_cast<int>(length));
    for (std::size_t i = 1; i < events.progressCalls.size(); ++i)
    {
        EXPECT_GE(events.progressCalls[i].first, events.progressCalls[i - 1].first);
    }
}

} // namespace
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test`
Expected: FAIL — no such target; the executor header does not exist.

- [ ] **Step 3: Write the executor header**

Create `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h`:

```cpp
#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable equivalent of FlashEcuMitsuM32rCanOperation (deleted by step 5
// tail wave 0). Preserves its exact wire bytes, chunk sizes, inter-exchange
// delays, response-validation rules, and log text.
//
// Two structural differences from the class it replaces, both forced by the
// dialog-free executor contract and both recorded in the flash qualification
// matrix:
//
//  - The MCU lookup, ROM-size floor and operation-support checks moved into
//    build_mitsu_colt_m32r_can_plan, so they run before any I/O.
//  - The two mid-operation QMessageBox gates (erase trigger, top-128KB
//    bootstrap) became declared ConfirmationSpecs that the desktop dialog
//    answers BEFORE execute() is called. Their presence on the plan means
//    "granted"; this executor never prompts.
class MitsuColtM32rCanExecutor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport,
                                         IClock& clock, const ICancellationToken& cancellation,
                                         IEventSink& events) override;
};

} // namespace fastecu::flash
```

- [ ] **Step 4: Write the connect and read implementation**

Create `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`. Task 7 appends the write path to this same file.

```cpp
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <format>
#include <span>

#include "src/algorithms/diagnostics/nrc_parser.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{
namespace
{

// Legacy field values, flash_ecu_mitsu_m32r_can_operation.h:56-57.
constexpr int kReadTimeoutMs = 500;
constexpr int kExtraLongTimeoutMs = 3000;

// Offset of the service byte in every reply: the 4-byte CAN reply id
// precedes it. The legacy code indexes received.at(4) throughout.
constexpr std::size_t kServiceOffset = 4;

struct Ctx
{
    ICanFlashTransport& transport;
    IClock& clock;
    const ICancellationToken& cancellation;
    IEventSink& events;
};

void info(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Info, message);
}

void error(Ctx& ctx, std::string_view message)
{
    ctx.events.log(LogLevel::Error, message);
}

// Legacy build_request, flash_ecu_mitsu_m32r_can_operation.cpp:58-64: every
// request carries the 4-byte big-endian 0x7E0 physical request id.
bytes::Bytes build_request(std::uint32_t request_id, bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, request_id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// The legacy NRC context is always received.mid(4, length - 1) -- everything
// from the service byte on. Reproduced exactly, including the off-by-one
// length the legacy expression produces.
std::string nrc_context(bytes::ByteView received)
{
    if (received.size() <= kServiceOffset)
    {
        return nrc_description({});
    }
    const std::size_t len = received.size() - 1 - kServiceOffset;
    return nrc_description(received.subspan(kServiceOffset, len));
}

// One write / delay / read exchange. Replaces the legacy
// write_serial_data_echo_check + delay() + read_serial_data() triple.
Result<bytes::Bytes> exchange(Ctx& ctx, std::uint32_t request_id, bytes::ByteView payload,
                              int delay_ms, int timeout_ms)
{
    if (ctx.cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before request");
    }
    const bytes::Bytes out = build_request(request_id, payload);
    if (Status written = ctx.transport.write(out, ctx.cancellation); !written)
    {
        return std::unexpected(written.error());
    }
    if (Status slept = ctx.clock.sleep(delay_ms, ctx.cancellation); !slept)
    {
        return std::unexpected(slept.error());
    }
    Result<std::optional<bytes::Bytes>> received = ctx.transport.read(timeout_ms,
                                                                     ctx.cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response within the read timeout");
    }
    return std::move(**received);
}

bool service_is(bytes::ByteView received, bytes::Byte service)
{
    return received.size() > kServiceOffset && received[kServiceOffset] == service;
}

// Legacy connect_bootloader, flash_ecu_mitsu_m32r_can_operation.cpp:66-168.
Status connect_bootloader(Ctx& ctx, const MitsuColtM32rCanPlan& family)
{
    using namespace MitsuColtCan;

    if (family.use_vendor_challenge)
    {
        // Lines 86-99.
        info(ctx, "Requesting vendor extension challenge seed...");
        Result<bytes::Bytes> received =
            exchange(ctx, family.request_id, MitsuColtCanVendorExt::buildChallengeSeedRequest(),
                     200, kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        // Line 91: length > 10 and the three selector bytes must match.
        if (received->size() <= 10 ||
            !service_is(*received, MitsuColtCanVendorExt::kServiceReadMemoryByAddress + 0x40) ||
            (*received)[5] != MitsuColtCanVendorExt::kVendorChallengeSelector ||
            (*received)[6] != MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction)
        {
            error(ctx, std::format("Wrong vendor challenge response from ECU: {}",
                                   nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "vendor challenge seed rejected");
        }

        const bytes::ByteView seed_bytes(received->data() + 7, 4);
        info(ctx, std::format("Received vendor seed: {}",
                              SsmProtocol::toHex(seed_bytes)));

        const std::uint32_t vendor_key = MitsuColtCanVendorExt::challengeInverseTransform(
            MitsuColtCanVendorExt::bytesToSeed(seed_bytes));
        const bytes::Bytes key_bytes = MitsuColtCanVendorExt::keyBytes(vendor_key);
        info(ctx, std::format("Calculated vendor key: {}",
                              SsmProtocol::toHex(key_bytes)));

        // Lines 106-116.
        info(ctx, "Sending vendor key to ECU...");
        received = exchange(ctx, family.request_id,
                            MitsuColtCanVendorExt::buildChallengeKey(vendor_key), 200,
                            kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (received->size() <= 6 ||
            !service_is(*received, MitsuColtCanVendorExt::kServiceReadMemoryByAddress + 0x40) ||
            (*received)[5] != MitsuColtCanVendorExt::kVendorChallengeSelector ||
            (*received)[6] != MitsuColtCanVendorExt::kVendorChallengeKeySubfunction)
        {
            error(ctx, std::format("Vendor challenge key rejected: {}",
                                   nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "vendor challenge key rejected");
        }
        info(ctx, "Vendor challenge accepted");
    }

    // Lines 119-129.
    info(ctx, "Starting diagnostic session...");
    Result<bytes::Bytes> received = exchange(
        ctx, family.request_id, buildDiagnosticSession(family.session_id), 50, kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 5 ||
        !service_is(*received, kServiceDiagnosticSession + 0x40) ||
        (*received)[5] != family.session_id)
    {
        error(ctx, std::format("Wrong response from ECU: {}", nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "diagnostic session rejected");
    }
    info(ctx, "Diagnostic session ok");

    // Line 131: the basic session needs no factory security access.
    if (family.session_id != kSessionBootload)
    {
        return {};
    }

    // Lines 136-165.
    info(ctx, "Requesting security seed...");
    received = exchange(ctx, family.request_id, buildSecurityAccessSeedRequest(), 200,
                        kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 9 || !service_is(*received, kServiceSecurityAccess + 0x40) ||
        (*received)[5] != 5)
    {
        error(ctx, std::format("Wrong response from ECU: {}", nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "security seed rejected");
    }

    const bytes::ByteView seed(received->data() + 6, 4);
    info(ctx, std::format("Received seed: {}", SsmProtocol::toHex(seed)));

    const bytes::Bytes key = seedKey(seed);
    info(ctx, std::format("Calculated seed key: {}", SsmProtocol::toHex(key)));

    info(ctx, "Sending seed key to ECU...");
    received = exchange(ctx, family.request_id, buildSecurityAccessKey(key), 200,
                        kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 5 || !service_is(*received, kServiceSecurityAccess + 0x40) ||
        (*received)[5] != 6)
    {
        error(ctx, std::format("Wrong response from ECU: {}", nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "security key rejected");
    }
    info(ctx, "Security access ok");

    return {};
}

// Legacy readFlashRange, flash_ecu_mitsu_m32r_can_operation.cpp:170-211.
// Progress is reported as (bytes done, bytes total) rather than the legacy
// integer percentage; the dialog converts. This preserves the emission
// points exactly -- one per chunk, after the chunk is appended.
Result<bytes::Bytes> read_flash_range(Ctx& ctx, const MitsuColtM32rCanPlan& family,
                                      std::uint32_t start_addr, std::uint32_t length)
{
    using namespace MitsuColtCan;

    bytes::Bytes data;
    data.reserve(length);
    const std::uint32_t end_addr = start_addr + length;

    for (std::uint32_t addr = start_addr; addr < end_addr;)
    {
        if (ctx.cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "read cancelled");
        }

        const std::uint32_t remaining = end_addr - addr;
        const auto chunk_len = static_cast<bytes::Byte>(
            remaining < kFlashReadBlockSize ? remaining : kFlashReadBlockSize);

        Result<bytes::Bytes> received =
            exchange(ctx, family.request_id, buildReadMemoryByAddress(addr, chunk_len), 50,
                     kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        // Line 196: length must cover header + service + payload.
        if (received->size() < kServiceOffset + 1u + chunk_len ||
            !service_is(*received, kServiceReadMemoryByAddress + 0x40))
        {
            error(ctx, std::format("Wrong response from ECU at 0x{:x}: {}", addr,
                                   nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "read chunk rejected");
        }

        data.insert(data.end(), received->begin() + kServiceOffset + 1,
                    received->begin() + kServiceOffset + 1 + chunk_len);
        addr += chunk_len;

        ctx.events.progress(static_cast<int>(addr - start_addr), static_cast<int>(length));
    }

    return data;
}

} // namespace

Result<FlashExecutionResult> MitsuColtM32rCanExecutor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status matched = check_family_transport_match(plan, FlashFamily::MitsuColtM32rCan,
                                                      TransportKind::CanIso15765);
        !matched)
    {
        return std::unexpected(matched.error());
    }

    auto *can = dynamic_cast<ICanFlashTransport *>(&transport);
    if (can == nullptr)
    {
        return fail(ErrorKind::Internal, "transport is not an ICanFlashTransport");
    }

    const auto& family = std::get<MitsuColtM32rCanPlan>(plan.family_plan());
    Ctx ctx{*can, clock, cancellation, events};

    if (Status configured = can->configure(Iso15765Config{family.bitrate, family.request_id,
                                                          family.response_id,
                                                          family.extended_id});
        !configured)
    {
        return std::unexpected(configured.error());
    }
    if (Status opened = can->open(); !opened)
    {
        return std::unexpected(opened.error());
    }

    // Legacy line 35.
    info(ctx, "Connecting to Mitsubishi Colt CZT M32R CAN bootloader, please wait...");
    if (Status connected = connect_bootloader(ctx, family); !connected)
    {
        return std::unexpected(connected.error());
    }

    if (plan.operation() == FlashOperation::Read)
    {
        // Legacy lines 42-45 and 215-228.
        events.notice("Reading ROM, please wait...");
        info(ctx, "Reading ROM from ECU using CAN");
        events.progress(0, static_cast<int>(plan.transfer_region().length));
        info(ctx, "Start reading ROM, please wait...");

        Result<bytes::Bytes> rom = read_flash_range(ctx, family, plan.transfer_region().start,
                                                    plan.transfer_region().length);
        if (!rom)
        {
            return std::unexpected(rom.error());
        }
        info(ctx, "ROM read complete");
        return FlashExecutionResult{FlashOperation::Read, std::move(*rom)};
    }

    return fail(ErrorKind::Unsupported, "write is implemented in the next task");
}

} // namespace fastecu::flash
```

- [ ] **Step 5: Extend the BUILD file**

Append to `src/backend/flash/ecu/BUILD.bazel`:

```python
# Portable. No Qt, no platform, no filesystem, no thread.
cc_library(
    name = "mitsu_colt_m32r_can_executor",
    srcs = ["mitsu_colt_m32r_can_executor.cpp"],
    hdrs = ["mitsu_colt_m32r_can_executor.h"],
    deps = [
        "//src/algorithms/diagnostics",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/colt",
        "//src/algorithms/protocol/ssm",
        "//src/backend/flash:flash_executor",
        "//src/backend/flash:flash_plan",
        "//src/backend/flash:flash_types",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "mitsu_colt_m32r_can_executor_test",
    srcs = ["mitsu_colt_m32r_can_executor_test.cpp"],
    deps = [
        ":mitsu_colt_m32r_can_executor",
        ":mitsu_colt_m32r_can_plan",
        "//src/algorithms/protocol/colt",
        "//src/backend/flash:flash_validation",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_clock",
    ],
)
```

Add `"mitsu_colt_m32r_can_executor"` to the `src/backend/flash/ecu` set in `PORTABLE_ROOTS`, and `"//src/backend/flash/ecu:mitsu_colt_m32r_can_executor"` to the genquery list in `BUILD.bazel`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
bazel test --config=release //src/backend/flash/ecu:all --test_output=errors
```

Expected: all PASS. If `MitsuColtM32rCanExecutor, RejectsAPlanFromAnotherFamily` fails, the guard ordering is wrong — `check_family_transport_match` must run before `configure()`.

- [ ] **Step 7: Confirm the executor stays Qt-free**

Run: `bazel test --config=release //:portable_closure --test_output=all`
Expected: PASS, with the executor target listed in the closure.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/ecu BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(flash): portable Mitsu Colt M32R CAN executor, read path

Ports connect_bootloader(), readFlashRange() and read_mem() from
FlashEcuMitsuM32rCanOperation. Wire bytes, chunk sizes, inter-exchange
delays, response-validation rules and log text are preserved; every
exchange cites the legacy line it came from.

Progress is reported as (done, total) bytes rather than a precomputed
percentage -- the emission points are unchanged and the dialog converts."
```

---

### Task 7: Portable executor — write path and top-region bootstrap

**Files:**
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`
- Test: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`

**Interfaces:**
- Consumes: everything Task 6 produced, plus `MitsuColtCan::{buildRequestDownload, buildTransferDataFrames, buildRoutineCheckCrc, buildRoutineErase, buildRequestReflashUnlock, checksum, kErasePageRoutine, kWritePageRoutine, kEraseRedirectRoutine, kWriteRedirectRoutine, kCrcTransferAddress, kCrcTransferSize, kEraseRoutineRamAddr, kWriteRoutineRamAddr, kTopRegionStart, kTopRegionLength, kUserspaceStart, kUserspaceEnd}`.
- Produces: no new public symbols — `execute()`'s `FlashOperation::Write` branch becomes functional.

- [ ] **Step 1: Write the failing write tests**

Append to `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`, inside the same anonymous namespace:

```cpp
fastecu::flash::FlashPlan writePlan(bytes::Bytes rom)
{
    auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol,
                                               "M32R_384KB_1block", false, std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Scripts the bootload handshake: session 0x85 then factory security access.
void scriptBootloadHandshake(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));

    transport.expectWrite(request(MitsuColtCan::buildSecurityAccessSeedRequest()));
    transport.queueRead(response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    transport.expectWrite(request(MitsuColtCan::buildSecurityAccessKey(
        MitsuColtCan::seedKey(seed))));
    transport.queueRead(response({0x67, 0x06}));
}

// Scripts one upload_and_commit(start, data): RequestDownload, the
// TransferData chunks, the CRC RequestDownload + TransferData, and the
// RoutineControl CRC check.
void scriptUploadAndCommit(ScriptedCanFlashTransport& transport, std::uint32_t start,
                           bytes::ByteView data)
{
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(start, data.size())));
    transport.queueRead(response({0x74}));

    for (const bytes::Bytes& chunk : MitsuColtCan::buildTransferDataFrames(data))
    {
        transport.expectWrite(request(chunk));
        transport.queueRead(response({0x76}));
    }

    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize)));
    transport.queueRead(response({0x74}));

    const std::uint16_t crc = MitsuColtCan::checksum(data);
    const bytes::Bytes crcData{static_cast<bytes::Byte>((crc >> 8) & 0xff),
                               static_cast<bytes::Byte>(crc & 0xff)};
    transport.expectWrite(request(MitsuColtCan::buildTransferDataFrames(crcData).front()));
    transport.queueRead(response({0x76}));

    transport.expectWrite(request(MitsuColtCan::buildRoutineCheckCrc(start)));
    transport.queueRead(response({0x71}));
}

// Scripts a readFlashRange over [start, start+length) returning `fill`.
void scriptFlashRead(ScriptedCanFlashTransport& transport, std::uint32_t start,
                     std::uint32_t length, bytes::Byte fill)
{
    for (std::uint32_t addr = start; addr < start + length;
         addr += MitsuColtCan::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = start + length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < MitsuColtCan::kFlashReadBlockSize ? remaining
                                                          : MitsuColtCan::kFlashReadBlockSize);
        transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(addr, chunk)));
        bytes::Bytes reply = response({0x63});
        reply.insert(reply.end(), chunk, fill);
        transport.queueRead(reply);
    }
}

TEST(MitsuColtM32rCanExecutor, WriteSkipsBootstrapWhenTheTopRegionAlreadyMatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // Top region in the ROM image is all 0xEE, and the ECU reports 0xEE too.
    bytes::Bytes rom(MitsuColtCan::kTopRegionEnd, 0x00);
    std::fill(rom.begin() + MitsuColtCan::kTopRegionStart, rom.end(), 0xEE);
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7b}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineErase()));
    transport.queueRead(response({0x71}));
    scriptUploadAndCommit(
        transport, MitsuColtCan::kUserspaceStart,
        bytes::ByteView(rom.data() + MitsuColtCan::kUserspaceStart,
                        MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.messages,
                Contains(std::string("Top 128KB already matches, no bootstrap needed")));
    EXPECT_THAT(events.messages, Contains(std::string("Userspace flash written")));
    EXPECT_FALSE(result->read_bytes.has_value());
}

TEST(MitsuColtM32rCanExecutor, WriteRunsTheBootstrapWhenTheTopRegionDiffers)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    bytes::Bytes rom(MitsuColtCan::kTopRegionEnd, 0x00);
    std::fill(rom.begin() + MitsuColtCan::kTopRegionStart, rom.end(), 0xEE);
    auto plan = writePlan(rom);
    const bytes::ByteView wantedTop(rom.data() + MitsuColtCan::kTopRegionStart,
                                    MitsuColtCan::kTopRegionLength);

    scriptBootloadHandshake(transport);
    // ECU reports 0xFF: mismatch, so the bootstrap runs.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kEraseRedirectRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWriteRedirectRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7b}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineErase()));
    transport.queueRead(response({0x71}));
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, wantedTop);
    // Verify read-back returns what was written.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    // Then the ordinary write proceeds.
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7b}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineErase()));
    transport.queueRead(response({0x71}));
    scriptUploadAndCommit(
        transport, MitsuColtCan::kUserspaceStart,
        bytes::ByteView(rom.data() + MitsuColtCan::kUserspaceStart,
                        MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.messages,
                Contains(std::string("Top 128KB mismatch, bootstrapping via redirect routines...")));
    EXPECT_THAT(events.messages, Contains(std::string("Top 128KB verified")));
}

TEST(MitsuColtM32rCanExecutor, WriteFailsWhenTheTopRegionVerifyMismatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    bytes::Bytes rom(MitsuColtCan::kTopRegionEnd, 0x00);
    std::fill(rom.begin() + MitsuColtCan::kTopRegionStart, rom.end(), 0xEE);
    auto plan = writePlan(rom);
    const bytes::ByteView wantedTop(rom.data() + MitsuColtCan::kTopRegionStart,
                                    MitsuColtCan::kTopRegionLength);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kEraseRedirectRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWriteRedirectRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7b}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineErase()));
    transport.queueRead(response({0x71}));
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, wantedTop);
    // Verify read-back still reports 0xFF: the write did not take.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(events.messages,
                Contains(std::string("Top 128KB verify failed after redirect write")));
}

TEST(MitsuColtM32rCanExecutor, WriteStopsWhenTheReflashUnlockIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    bytes::Bytes rom(MitsuColtCan::kTopRegionEnd, 0x00);
    std::fill(rom.begin() + MitsuColtCan::kTopRegionStart, rom.end(), 0xEE);
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7f, 0x3b, 0x33}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:451.
    EXPECT_THAT(events.messages, Contains(HasSubstr("Reflash unlock rejected: ")));
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test --test_output=errors`
Expected: FAIL — the write branch returns `Unsupported`.

- [ ] **Step 3: Implement `upload_and_commit`**

Add to the anonymous namespace in `mitsu_colt_m32r_can_executor.cpp`, after `read_flash_range`:

```cpp
// Legacy upload_and_commit, flash_ecu_mitsu_m32r_can_operation.cpp:231-297.
Status upload_and_commit(Ctx& ctx, const MitsuColtM32rCanPlan& family, std::uint32_t start,
                         bytes::ByteView data)
{
    using namespace MitsuColtCan;

    // Lines 238-246.
    Result<bytes::Bytes> received =
        exchange(ctx, family.request_id, buildRequestDownload(start, data.size()), 50,
                 kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, kServiceRequestDownload + 0x40))
    {
        error(ctx, std::format("RequestDownload to 0x{:x} rejected: {}", start,
                               nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "RequestDownload rejected");
    }

    // Lines 248-260.
    for (const bytes::Bytes& chunk : buildTransferDataFrames(data))
    {
        received = exchange(ctx, family.request_id, chunk, 50, kReadTimeoutMs);
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (!service_is(*received, kServiceTransferData + 0x40))
        {
            error(ctx, std::format("TransferData to 0x{:x} rejected: {}", start,
                                   nrc_context(*received)));
            return fail(ErrorKind::BadResponse, "TransferData rejected");
        }
    }

    // Lines 262-270.
    received = exchange(ctx, family.request_id,
                        buildRequestDownload(kCrcTransferAddress, kCrcTransferSize), 50,
                        kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, kServiceRequestDownload + 0x40))
    {
        error(ctx, std::format("RequestDownload for checksum rejected: {}",
                               nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "checksum RequestDownload rejected");
    }

    // Lines 272-284: big-endian 16-bit running sum, one TransferData frame.
    const std::uint16_t crc = checksum(data);
    const bytes::Bytes crc_data{static_cast<bytes::Byte>((crc >> 8) & 0xff),
                                static_cast<bytes::Byte>(crc & 0xff)};
    received = exchange(ctx, family.request_id, buildTransferDataFrames(crc_data).front(), 50,
                        kReadTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, kServiceTransferData + 0x40))
    {
        error(ctx, std::format("TransferData for checksum rejected: {}",
                               nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "checksum TransferData rejected");
    }

    // Lines 286-294: the CRC check gets the extra-long timeout.
    received = exchange(ctx, family.request_id, buildRoutineCheckCrc(start), 200,
                        kExtraLongTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, kServiceRoutineControl + 0x40))
    {
        error(ctx, std::format("RoutineControl CRC check for 0x{:x} rejected: {}", start,
                               nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "CRC RoutineControl rejected");
    }

    return {};
}

// Legacy: the unlock + erase-trigger pair that appears identically in
// ensureTopRegionWritten (lines 349-368) and write_mem (lines 445-464). The
// only difference between the two copies is the log-message prefix, so it is
// a parameter here rather than two transcriptions.
Status unlock_and_erase(Ctx& ctx, const MitsuColtM32rCanPlan& family,
                        std::string_view unlock_prefix, std::string_view erase_prefix)
{
    using namespace MitsuColtCan;

    Result<bytes::Bytes> received = exchange(ctx, family.request_id,
                                             buildRequestReflashUnlock(), 200,
                                             kExtraLongTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, kServiceRequestReflash + 0x40))
    {
        error(ctx, std::format("{}{}", unlock_prefix, nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "reflash unlock rejected");
    }

    received = exchange(ctx, family.request_id, buildRoutineErase(), 200, kExtraLongTimeoutMs);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!service_is(*received, kServiceRoutineControl + 0x40))
    {
        error(ctx, std::format("{}{}", erase_prefix, nrc_context(*received)));
        return fail(ErrorKind::BadResponse, "erase trigger rejected");
    }

    return {};
}
```

- [ ] **Step 4: Implement the top-region bootstrap and the write path**

Append to the same anonymous namespace:

```cpp
// Legacy ensureTopRegionWritten, flash_ecu_mitsu_m32r_can_operation.cpp:299-390.
// The mid-function confirm() at line 320 is gone: the desktop dialog answers
// ConfirmationSpec::Id::TopRegionBootstrap before execute() is called, and a
// declined gate means no plan is built at all.
Status ensure_top_region_written(Ctx& ctx, const MitsuColtM32rCanPlan& family,
                                 bytes::ByteView rom)
{
    using namespace MitsuColtCan;

    info(ctx, std::format("Checking top 128KB (0x{:x}-0x{:x})...", kTopRegionStart,
                          kTopRegionEnd));

    Result<bytes::Bytes> current_top =
        read_flash_range(ctx, family, kTopRegionStart, kTopRegionLength);
    if (!current_top)
    {
        return std::unexpected(current_top.error());
    }

    const bytes::ByteView wanted_top(rom.data() + kTopRegionStart, kTopRegionLength);
    if (std::ranges::equal(*current_top, wanted_top))
    {
        info(ctx, "Top 128KB already matches, no bootstrap needed");
        return {};
    }

    info(ctx, "Top 128KB mismatch, bootstrapping via redirect routines...");

    info(ctx, std::format("Uploading erase redirect routine to RAM 0x{:x}...",
                          kEraseRoutineRamAddr));
    if (Status uploaded = upload_and_commit(ctx, family, kEraseRoutineRamAddr,
                                            kEraseRedirectRoutine);
        !uploaded)
    {
        error(ctx, "Erase redirect routine upload failed");
        return uploaded;
    }

    info(ctx, std::format("Uploading write redirect routine to RAM 0x{:x}...",
                          kWriteRoutineRamAddr));
    if (Status uploaded = upload_and_commit(ctx, family, kWriteRoutineRamAddr,
                                            kWriteRedirectRoutine);
        !uploaded)
    {
        error(ctx, "Write redirect routine upload failed");
        return uploaded;
    }

    if (Status erased = unlock_and_erase(
            ctx, family, "Reflash unlock (top 128KB bootstrap) rejected: ",
            "Erase trigger (top 128KB bootstrap) rejected: ");
        !erased)
    {
        return erased;
    }
    info(ctx, "Carrier window erased");

    // The carrier address is kUserspaceStart, not kTopRegionStart: the
    // bootloader hard-validates RequestDownload targets into the userspace
    // window, and the redirect routines add the +0x058000 offset themselves.
    // See mitsu_colt_can_protocol.h's kEraseRedirectRoutine comment.
    if (Status written = upload_and_commit(ctx, family, kUserspaceStart, wanted_top); !written)
    {
        error(ctx, "Top 128KB redirect write failed");
        return written;
    }
    info(ctx, "Top 128KB written via redirect");

    Result<bytes::Bytes> verify_top =
        read_flash_range(ctx, family, kTopRegionStart, kTopRegionLength);
    if (!verify_top)
    {
        return std::unexpected(verify_top.error());
    }
    if (!std::ranges::equal(*verify_top, wanted_top))
    {
        error(ctx, "Top 128KB verify failed after redirect write");
        return fail(ErrorKind::BadResponse, "top region verify mismatch");
    }
    info(ctx, "Top 128KB verified");

    return {};
}

// Legacy write_mem, flash_ecu_mitsu_m32r_can_operation.cpp:392-476. The ROM
// size check at lines 398-402 moved into the plan builder, and the confirm()
// at line 433 became ConfirmationSpec::Id::EraseTrigger.
Status write_mem(Ctx& ctx, const MitsuColtM32rCanPlan& family, bytes::ByteView rom)
{
    using namespace MitsuColtCan;

    if (Status bootstrapped = ensure_top_region_written(ctx, family, rom); !bootstrapped)
    {
        return bootstrapped;
    }

    info(ctx, std::format("Uploading erase-page routine to RAM 0x{:x}...",
                          kEraseRoutineRamAddr));
    if (Status uploaded =
            upload_and_commit(ctx, family, kEraseRoutineRamAddr, kErasePageRoutine);
        !uploaded)
    {
        error(ctx, "Erase-page routine upload failed");
        return uploaded;
    }
    info(ctx, "Erase page uploaded");

    info(ctx, std::format("Uploading write-page routine to RAM 0x{:x}...",
                          kWriteRoutineRamAddr));
    if (Status uploaded =
            upload_and_commit(ctx, family, kWriteRoutineRamAddr, kWritePageRoutine);
        !uploaded)
    {
        error(ctx, "Write-page routine upload failed");
        return uploaded;
    }
    info(ctx, "Write page uploaded");

    // --- HIGH RISK STEP ---
    // This 12-byte ServiceRequestReflash(0x3B) payload is carried over
    // verbatim from externals/livemonitor/obdsessionwidget.cpp:180-181,
    // where the original author's own comment reads "caused bootloader
    // lockup" during their testing. The plan's EraseTrigger confirmation is
    // the gate; it has already been granted by the time execute() runs.
    if (Status erased = unlock_and_erase(ctx, family, "Reflash unlock rejected: ",
                                         "Erase trigger rejected: ");
        !erased)
    {
        return erased;
    }
    info(ctx, "Userspace flash erased");

    info(ctx, std::format("Writing ROM userspace 0x{:x}-0x{:x}...", kUserspaceStart,
                          kUserspaceEnd));
    const bytes::ByteView userspace(rom.data() + kUserspaceStart,
                                    kUserspaceEnd - kUserspaceStart);
    if (Status written = upload_and_commit(ctx, family, kUserspaceStart, userspace); !written)
    {
        error(ctx, "ROM userspace write failed");
        return written;
    }
    info(ctx, "Userspace flash written");

    return {};
}
```

Add `#include <algorithm>` and `#include <ranges>` at the top of the file.

- [ ] **Step 5: Replace the write stub in `execute()`**

Replace:

```cpp
    return fail(ErrorKind::Unsupported, "write is implemented in the next task");
```

with:

```cpp
    // Legacy lines 49-51.
    events.notice("Writing ROM, please wait...");
    info(ctx, "Writing ROM to ECU using CAN");

    // validate_and_build guarantees a Write plan carries an image, and the
    // plan builder guarantees it is at least kTopRegionEnd bytes.
    const bytes::Bytes& rom = *plan.image();
    if (Status written = write_mem(ctx, family, rom); !written)
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{plan.operation(), std::nullopt};
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test --test_output=errors`
Expected: all PASS, including the four new write cases and the ten read cases from Task 6.

- [ ] **Step 7: Run the full suite and the guards**

```bash
bazel test -k --config=release //... --test_output=errors
```

Expected: all PASS.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/ecu
git commit -m "feat(flash): Mitsu Colt M32R CAN executor, write path

Ports upload_and_commit(), ensureTopRegionWritten() and write_mem().
Wire bytes, chunk sizes, timeouts and log text preserved; every
exchange cites its legacy line.

The two mid-operation QMessageBox gates are gone: a dialog-free
executor cannot block for a human answer, so they became declared
ConfirmationSpecs the dialog answers before execute() runs. The
unlock + erase-trigger pair appeared twice in the legacy source
differing only in log prefix, so it is one function with the prefix
as a parameter rather than two transcriptions."
```

---

### Task 8: Rewrite the desktop dialog onto the portable executor

**Files:**
- Modify: `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.{h,cpp}`
- Modify: `src/ui/desktop/flash/ecu/BUILD.bazel`
- Test: `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can_dialog_test.cpp`

**Interfaces:**
- Consumes: `build_mitsu_colt_m32r_can_plan` (Task 5), `MitsuColtM32rCanExecutor` (Tasks 6-7), `FlashWorker`/`FlashWorkerResult` from `//src/platform/desktop/common/flash:flash_worker`, `DesktopCanFlashTransport` from `//src/platform/desktop/common/transport:flash_transports`, `QtClock` from `//src/platform/desktop/common/ports`.
- Produces: `FlashEcuMitsuM32rCan` keeps its existing constructor signature — `(SerialPortActions*, FileActions::EcuCalDefStructure*, const QString& cmd_type, QWidget* parent, bool useVendorChallenge)` — so `mainwindow.cpp` is untouched. `confirm()`, `showFailureDialog()` and `buildPlan()` are `virtual`/`protected` so the test can subclass.

- [ ] **Step 1: Write the failing dialog test**

Create `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can_dialog_test.cpp`:

```cpp
#include <QtTest>
#include <QApplication>

#include "src/backend/definitions/file_actions.h"
#include "src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h"

// Records confirmations and failure dialogs instead of showing modals, and
// answers each prompt from a scripted queue.
class TestableFlashEcuMitsuM32rCan : public FlashEcuMitsuM32rCan
{
  public:
    using FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan;

    QStringList confirmTitles;
    QList<int> confirmAnswers;
    QList<fastecu::ErrorKind> failureKinds;

  protected:
    int confirm(const QString& title, const QString& text, int buttons,
                int defaultButton) override
    {
        Q_UNUSED(text)
        Q_UNUSED(buttons)
        confirmTitles << title;
        return confirmAnswers.isEmpty() ? defaultButton : confirmAnswers.takeFirst();
    }

    void showFailureDialog(fastecu::ErrorKind kind, const QString& detail) override
    {
        Q_UNUSED(detail)
        failureKinds << kind;
    }
};

class TestFlashEcuMitsuM32rCanDialog : public QObject
{
    Q_OBJECT
  private slots:
    void readDeclinedAtIgnitionPromptStartsNoWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Cancel;

        dialog.run();

        QCOMPARE(dialog.confirmTitles.size(), 1);
        QCOMPARE(dialog.confirmTitles.at(0), QString("Connecting to ECU"));
        QVERIFY(dialog.failureKinds.isEmpty());
    }

    void unknownMcuIsRejectedBeforeAnyWorkerStarts()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "NOT_A_REAL_MCU";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::InvalidConfig);
    }

    void testWriteIsRejectedAsUnsupported()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "test_write", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::Unsupported);
    }

    void writeCollectsBothGatesBeforeBuildingAPlan()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x80000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr, false);
        // Ignition OK, erase trigger declined: stop before any plan is built.
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Cancel;

        dialog.run();

        QCOMPARE(dialog.confirmTitles.size(), 2);
        QCOMPARE(dialog.confirmTitles.at(1), QString("Erase trigger"));
        QVERIFY(dialog.failureKinds.isEmpty());
    }

    void writeAsksTheTopRegionGateAfterTheEraseGate()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x80000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Yes << QMessageBox::Cancel;

        dialog.run();

        QCOMPARE(dialog.confirmTitles.size(), 3);
        QCOMPARE(dialog.confirmTitles.at(2), QString("Top 128KB bootstrap"));
    }

    void romTooSmallIsRejectedAsInvalidConfig()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x80000 - 1, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Yes << QMessageBox::Yes;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::InvalidConfig);
    }
};

QTEST_MAIN(TestFlashEcuMitsuM32rCanDialog)
#include "flash_ecu_mitsu_m32r_can_dialog_test.moc"
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/ui/desktop/flash/ecu:test_flash_ecu_mitsu_m32r_can_dialog`
Expected: FAIL — no such target.

- [ ] **Step 3: Rewrite the dialog header**

Replace `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h`'s private section and forward declarations. Delete `class FlashEcuMitsuM32rCanOperation;` and the `m_operation` member; add:

```cpp
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/error.h"
#include "src/platform/desktop/common/flash/flash_worker.h"
```

```cpp
  protected:
    // Virtual so the dialog test can answer prompts from a script and record
    // failures instead of showing modals -- same shape as the EEPROM pair's
    // dialogs.
    virtual int confirm(const QString& title, const QString& text, int buttons,
                        int defaultButton);
    virtual void showFailureDialog(fastecu::ErrorKind kind, const QString& detail);

  private:
    fastecu::Result<fastecu::flash::FlashPlan> buildPlan();
    std::unique_ptr<fastecu::flash::FlashWorker> makeWorker(fastecu::flash::FlashPlan plan);
    void onWorkerFinished(fastecu::flash::FlashWorkerResult result);

    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;
    bool useVendorChallenge = false;
    SerialPortActions *serial;
    std::unique_ptr<fastecu::flash::FlashWorker> worker_;
    QEventLoop *loop_ = nullptr;

    void closeEvent(QCloseEvent *event) override;
    void set_progressbar_value(int value);

    std::unique_ptr<Ui::EcuOperationsWindow> ui;
```

- [ ] **Step 4: Rewrite `run()` and add the new members**

Replace the body of `run()` in `flash_ecu_mitsu_m32r_can.cpp` and add the helpers. The confirmation collection order is: ignition, then (for a write) erase trigger, then top-region bootstrap — every gate answered before a plan exists.

```cpp
void FlashEcuMitsuM32rCan::run()
{
    this->show();
    set_progressbar_value(0);

    const int ret = confirm(
        tr("Connecting to ECU"),
        tr("Turn ignition ON and press OK to start initializing connection to ECU"),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
    if (ret != QMessageBox::Ok)
    {
        emit LOG_D("Operation canceled", true, true);
        close();
        return;
    }

    if (cmd_type == "write")
    {
        // Both gates are collected here, before any plan is built, because a
        // synchronous dialog-free executor cannot block mid-run for an
        // answer. Text is carried over verbatim from the legacy operation
        // class (flash_ecu_mitsu_m32r_can_operation.cpp:433-438 and 320-328).
        const int eraseReply =
            confirm(tr("Erase trigger"),
                    tr("About to send the flash-erase trigger command. This exact "
                       "sequence is known to have locked up the bootloader during the "
                       "original implementation's testing. Only continue if this is a "
                       "bench/spare ECU with a recovery path available.\n\nContinue?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (eraseReply != QMessageBox::Yes)
        {
            emit LOG_I("Erase trigger canceled by user", true, true);
            close();
            return;
        }

        const int bootstrapReply =
            confirm(tr("Top 128KB bootstrap"),
                    tr("The top 128KB (0x60000-0x80000) may not match the ROM being "
                       "written. If it does not, it needs a one-time bootstrap pass "
                       "through custom erase/write redirect helpers, outside the range "
                       "the vendor bootloader normally allows. This sends the same "
                       "high-risk erase trigger sequence used for the main write, once "
                       "then and once more for the main write that follows. Only "
                       "continue on a bench/spare ECU with a recovery path available."
                       "\n\nContinue?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (bootstrapReply != QMessageBox::Yes)
        {
            emit LOG_I("Top 128KB bootstrap canceled by user", true, true);
            close();
            return;
        }
    }

    fastecu::Result<fastecu::flash::FlashPlan> planResult = buildPlan();
    if (!planResult.has_value())
    {
        showFailureDialog(planResult.error().kind,
                          QString::fromStdString(planResult.error().detail));
        close();
        return;
    }

    worker_ = makeWorker(std::move(*planResult));

    connect(worker_.get(), &fastecu::flash::FlashWorker::logEvent, this,
            [this](int level, const QString& message)
            {
                switch (static_cast<fastecu::LogLevel>(level))
                {
                case fastecu::LogLevel::Error:
                    emit LOG_E(message, true, true);
                    break;
                case fastecu::LogLevel::Warning:
                    emit LOG_W(message, true, true);
                    break;
                case fastecu::LogLevel::Info:
                    emit LOG_I(message, true, true);
                    break;
                case fastecu::LogLevel::Debug:
                    emit LOG_D(message, true, true);
                    break;
                }
            });
    connect(worker_.get(), &fastecu::flash::FlashWorker::progressChanged, this,
            [this](int done, int total)
            {
                const int pct =
                    total > 0 ? static_cast<int>((static_cast<qint64>(done) * 100) / total)
                              : done;
                set_progressbar_value(pct);
            });
    connect(worker_.get(), &fastecu::flash::FlashWorker::finished, this,
            &FlashEcuMitsuM32rCan::onWorkerFinished);

    QEventLoop loop;
    loop_ = &loop;
    worker_->start();
    loop.exec();
    loop_ = nullptr;
}

void FlashEcuMitsuM32rCan::onWorkerFinished(fastecu::flash::FlashWorkerResult result)
{
    worker_.reset();
    emit external_logger("Finished");

    if (result.success)
    {
        if (result.read_bytes.has_value())
        {
            ecuCalDef->FullRomData =
                bytes::toQByteArray(bytes::ByteView(*result.read_bytes));
        }
        QMessageBox::information(this, tr("ECU Operation"),
                                 "ECU operation was succesful, press OK to exit");
        close();
        return;
    }

    if (result.error_kind == fastecu::ErrorKind::Cancelled)
    {
        close();
        return;
    }

    showFailureDialog(result.error_kind, result.error_detail);
    if (loop_)
    {
        loop_->quit();
    }
}

fastecu::Result<fastecu::flash::FlashPlan> FlashEcuMitsuM32rCan::buildPlan()
{
    using fastecu::flash::FlashOperation;

    FlashOperation operation = FlashOperation::Read;
    std::optional<bytes::Bytes> image;
    if (cmd_type == "write")
    {
        operation = FlashOperation::Write;
        image = bytes::fromQByteArray(ecuCalDef->FullRomData);
    }
    else if (cmd_type == "test_write")
    {
        operation = FlashOperation::TestWrite;
    }

    return fastecu::flash::build_mitsu_colt_m32r_can_plan(
        operation, ecuCalDef->FlashMethod.toStdString(), ecuCalDef->McuType.toStdString(),
        useVendorChallenge, std::move(image));
}

std::unique_ptr<fastecu::flash::FlashWorker> FlashEcuMitsuM32rCan::makeWorker(
    fastecu::flash::FlashPlan plan)
{
    return std::make_unique<fastecu::flash::FlashWorker>(
        std::move(plan), std::make_unique<fastecu::flash::MitsuColtM32rCanExecutor>(),
        std::make_unique<fastecu::flash::DesktopCanFlashTransport>(serial),
        std::make_unique<QtClock>());
}

int FlashEcuMitsuM32rCan::confirm(const QString& title, const QString& text, int buttons,
                                  int defaultButton)
{
    return QMessageBox::warning(this, title, text,
                                static_cast<QMessageBox::StandardButtons>(buttons),
                                static_cast<QMessageBox::StandardButton>(defaultButton));
}

void FlashEcuMitsuM32rCan::showFailureDialog(fastecu::ErrorKind kind, const QString& detail)
{
    using fastecu::ErrorKind;

    switch (kind)
    {
    case ErrorKind::InvalidConfig:
    case ErrorKind::Unsupported:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU flash configuration is invalid or unsupported for this "
                                "operation. Check the ROM definition and selected protocol, "
                                "then try again."));
        break;
    case ErrorKind::Disconnected:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("Lost connection to the adapter or ECU. Check the "
                                "cable/adapter connection, press OK to exit and try again."));
        break;
    case ErrorKind::Timeout:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU did not respond in time, press OK to exit and try "
                                "again."));
        break;
    case ErrorKind::BadResponse:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU returned an unexpected or rejected response, press OK "
                                "to exit and try again."));
        break;
    case ErrorKind::Cancelled:
        break;
    case ErrorKind::Internal:
        // Verbatim legacy text -- the only failure message the pre-rewrite
        // dialog ever showed.
        QMessageBox::warning(this, tr("ECU Operation"),
                             "ECU operation failed, press OK to exit and try again");
        break;
    }

    emit LOG_E(QString("ECU operation failed (%1): %2")
                   .arg(QString::fromUtf8(fastecu::to_string(kind)), detail),
               true, true);
}

void FlashEcuMitsuM32rCan::closeEvent(QCloseEvent *event)
{
    if (worker_)
    {
        worker_->requestStop();
    }
    if (loop_)
    {
        loop_->quit();
    }
    QDialog::closeEvent(event);
}
```

Update the includes at the top of the file:

```cpp
#include "src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h"

#include <utility>

#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"
```

- [ ] **Step 5: Add the test target and the new deps**

In `src/ui/desktop/flash/ecu/BUILD.bazel`, add to the `ecu` library's `deps`:

```python
        "//src/backend/flash:flash_plan",
        "//src/backend/flash/ecu:mitsu_colt_m32r_can_executor",
        "//src/backend/flash/ecu:mitsu_colt_m32r_can_plan",
        "//src/backend/ports",
        "//src/platform/desktop/common/flash:flash_worker",
        "//src/platform/desktop/common/ports",
        "//src/platform/desktop/common/transport:flash_transports",
```

and append:

```python
fastecu_qttest(
    name = "test_flash_ecu_mitsu_m32r_can_dialog",
    src = "flash_ecu_mitsu_m32r_can_dialog_test.cpp",
    copts = ["-DQT_WIDGETS_LIB"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [
        ":ecu",
        "//src/backend/definitions",
        "//src/backend/ports",
    ],
)
```

Load `fastecu_qttest` in that file's `load(...)` statement if it is not already imported.

- [ ] **Step 6: Run the dialog tests**

Run: `bazel test --config=release //src/ui/desktop/flash/ecu:test_flash_ecu_mitsu_m32r_can_dialog --test_output=errors`
Expected: all six cases PASS.

- [ ] **Step 7: Verify the app still builds and `mainwindow.cpp` was not touched**

```bash
bazel build -k --config=release //:fastecu
git diff --stat src/ui/desktop/mainwindow.cpp
```

Expected: build succeeds; the diff is empty — the constructor signature was preserved deliberately.

- [ ] **Step 8: Commit**

```bash
git add src/ui/desktop/flash/ecu
git commit -m "refactor(ui): drive Mitsu Colt M32R CAN from the portable executor

Replaces the FlashOperationWorker wiring with buildPlan + FlashWorker +
MitsuColtM32rCanExecutor + DesktopCanFlashTransport, following the
EEPROM dialogs' shape. The constructor signature is unchanged, so
mainwindow.cpp is untouched.

Both write gates are now collected before a plan is built, since a
dialog-free executor cannot block mid-run for an answer. The top-128KB
prompt is therefore shown whenever a write is requested, not only when
the region actually mismatches; its wording is adjusted from 'does not
match' to 'may not match' to stay truthful. Recorded in the flash
qualification matrix."
```

---

### Task 9: Delete the legacy class and close out wave 0

**Files:**
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.{h,cpp}`, `src/platform/desktop/common/flash/legacy/flash_ecu_mitsu_m32r_can_operation_test.cpp`, `src/algorithms/protocol/colt/qt_colt.h`
- Modify: `scripts/check-legacy-flash-drain.py`, `src/platform/desktop/common/flash/legacy/BUILD.bazel`, `src/algorithms/protocol/colt/BUILD.bazel`, `docs/flash-qualification-matrix.md`, `docs/modularization-plan.md`

**Interfaces:**
- Consumes: everything Tasks 5-8 produced.
- Produces: nothing. This task removes.

- [ ] **Step 1: Confirm the legacy class has no remaining callers**

```bash
grep -rn "FlashEcuMitsuM32rCanOperation" src apps tests
grep -rn "qt_colt.h\|colt:qt_compat" src apps tests
```

Expected: the first prints only the files about to be deleted. The second prints only `src/platform/desktop/common/flash/legacy/BUILD.bazel` and the deleted test — Task 8 removed the last `#include`.

- [ ] **Step 2: Delete the legacy sources and the colt shim**

```bash
git rm src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.h \
       src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.cpp \
       src/platform/desktop/common/flash/legacy/flash_ecu_mitsu_m32r_can_operation_test.cpp \
       src/algorithms/protocol/colt/qt_colt.h
```

In `src/platform/desktop/common/flash/legacy/BUILD.bazel`, delete the whole `fastecu_qttest(name = "test_flash_ecu_mitsu_m32r_can_operation", ...)` block and the `"//src/algorithms/protocol/colt:qt_compat",` dep. In `src/algorithms/protocol/colt/BUILD.bazel`, delete the `qt_cc_library(name = "qt_compat", ...)` block.

The three assertions the deleted legacy test carried are already covered: `colt384kReadRangeStartsAtUserspace` by `MitsuColtM32rCanPlan.ReadPlanCoversTheFirstFlashBlock`, `iso15765Request_prependsBigEndianSourceAddress` by every `request()` helper assertion in the executor suite, and the three connect/stop/vendor cases by `MitsuColtM32rCanExecutor`'s handshake tests. Confirm each mapping before deleting rather than assuming it.

- [ ] **Step 3: Shrink the ratchet**

In `scripts/check-legacy-flash-drain.py`, delete this line from `REMAINING`:

```python
    "ecu/flash_ecu_mitsu_m32r_can_operation.cpp",
```

- [ ] **Step 4: Verify the ratchet moved and everything still builds**

```bash
bazel test --config=release //:legacy_flash_drain --test_output=all
bazel build -k --config=release //:fastecu //tests/...
bazel test -k --config=release //... --test_output=errors
```

Expected: the drain prints `OK: 26 families remaining, none added.`; the build succeeds; all tests PASS.

- [ ] **Step 5: Update the qualification matrix**

In `docs/flash-qualification-matrix.md`, replace the `FlashEcuMitsuM32rCan` row with:

```markdown
| FlashEcuMitsuM32rCan | ECU | ISO-15765 | read, write | yes | `mitsu_colt_m32r_can_plan_test`, `mitsu_colt_m32r_can_executor_test`, `test_flash_ecu_mitsu_m32r_can_dialog` @ step 5 tail wave 0 | experimental | — | Portable `MitsuColtM32rCanPlan`/`MitsuColtM32rCanExecutor` (`src/backend/flash/ecu/`) replacing the deleted `FlashEcuMitsuM32rCanOperation`. Serves both `mitsu_ecu_m32r_can` and `mitsu_ecu_m32r_can_vendor_ext`; the vendor security-access challenge is a plan flag, not a separate family. **Four deliberate divergences from the legacy implementation.** (1) `test_write` now returns `Unsupported`; the legacy class silently returned success after performing only the diagnostic-session handshake, and protocols.cfg declares `test_write=no` — same shape as the step-5c EEPROM write gap. (2) Unknown MCU type, ROM shorter than `kTopRegionEnd`, and unsupported-operation rejection moved into the plan builder, so they now fail before the port is opened rather than after the bootloader handshake. (3) The two mid-operation confirmations (erase trigger, top-128KB bootstrap) are collected by the dialog before execution; a dialog-free executor cannot block mid-run for an answer. (4) Consequently the top-128KB prompt appears on every write, not only when the region mismatches, and its wording changed from "does not match" to "may not match". A [bench-qualification checklist](colt_czt_47110032_can_bench_checklist.md) exists but gates future real-vehicle use rather than reporting completed testing, so `hardware_status` stays `experimental`. |
```

- [ ] **Step 6: Record the wave in the modularization plan**

In `docs/modularization-plan.md`, under the step-5 status list, replace the paragraph beginning "Steps 6 (thin desktop shell) and 7 (Android seam) have not started." with:

```markdown
Steps 6 (thin desktop shell) and 7 (Android seam) have not started. The
per-family flash tail is under way — see the
[tail design](superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md)
for the eight-wave sequencing:

- Wave 0 `FlashEcuMitsuM32rCan` — merged 2026-08-08. Installs
  `//:legacy_flash_drain`, which ratchets the remaining families from 27
  down to zero over waves 1-7.
```

- [ ] **Step 7: Run every guard and the docs linter**

```bash
bazel test -k --config=release //... --test_output=errors
prek run --all-files
```

Expected: all PASS, including `//:legacy_flash_drain`, `//:portable_closure`, `//:serial_compat_allowlist`, and lychee on the two edited documents.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor: delete the legacy Mitsu Colt M32R CAN operation

Wave 0 of the step 5 tail is complete. The legacy Qt operation class,
its test, and the colt qt_compat shim its last two callers needed are
deleted; //:legacy_flash_drain drops from 27 families to 26.

Every assertion the deleted test carried is covered by the new plan,
executor and dialog suites. Four deliberate divergences from the legacy
implementation are recorded in the flash qualification matrix, which
flips this family to portable=yes / experimental."
```

---

## Post-wave verification

Run once, before opening the PR:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
prek run --all-files
scripts/coverage-local.sh
```

- `//:legacy_flash_drain` must print `OK: 26 families remaining, none added.`
- `//:serial_compat_allowlist` must still print 13 entries — wave 0 does not touch it; the package-level entry only moves in wave 7.
- New-code coverage must be `>=80%` for `src/backend/flash/ecu/` and the rewritten dialog.
