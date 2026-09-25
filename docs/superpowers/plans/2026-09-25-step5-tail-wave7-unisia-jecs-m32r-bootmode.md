# Step 5 Tail Wave 7 — Unisia Jecs M32R Bootmode and Legacy Teardown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the last legacy flash family, `FlashEcuSubaruUnisiaJecsM32rBootModeOperation`, to portable plans and executors, then delete the legacy flash package and every guard that fenced it.

**Architecture:** Bootmode Read reuses the 6c-3 `SubaruUnisiaJecsM32rKline` plan and executor. Bootmode Write becomes two `FlashAttempt`s in one workflow: a kernel-upload attempt (39063 baud, even parity, VPP+MOD1 lines), then a `RemoveMod1` prompt, then an erase-and-program attempt (19200 baud, no parity, VPP only). PR 7a lands the migration; PR 7b deletes `common/flash/legacy/`, the drain ratchet, `ssm:qt_compat`, and closes the wave docs.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest (portable targets), QtTest + gmock (desktop), Python guard scripts, `prek`.

**Spec:** [wave 7 design](../specs/2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md). Read it before starting; Task 1 amends it.

## Global Constraints

- Backend targets are portable: no Qt, no threads, no filesystem. Every new `cc_library` in `src/backend/flash/ecu` is registered **by name** in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`).
- Backend operations return `fastecu::Result<T>` / `Status`, checked with `.has_value()`, never implicit `operator bool`. Exceptions never cross a port. `ErrorKind` is closed — add no value.
- Bytes are `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView`.
- Tester ID `0xF0`, target ID `0x10` for every SSM frame.
- Timing budgets are legacy's, cited per constant with `…_bootmode_operation.cpp:<line>`.
- Hardware status stays `experimental`. Nothing is marked qualified.
- Ratchet lists only shrink: `REMAINING`, `FROZEN`, and the `serial_qt_compat` visibility list may lose entries, never gain them.
- Markdown cross-references are links with human-readable text, not backticked paths.
- Git: work on a branch, never `master`. Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc
  ```
- Per-commit gate: `prek run --all-files` passes. Per-PR gate: `bazel test --config=release //...` and `bazel run //:clang_tidy_report_changed` are clean.

## Review Focus

Inputs the spec implies that no task would otherwise exercise, most likely first. Each has a pinning test in the owning task.

1. **A kernel file that is already a multiple of 128 bytes** gets no extra padding block — Task 5, `KernelAlreadyAlignedIsNotPadded`.
2. **An empty kernel file** fails before any prompt, not at upload — Task 5, `EmptyKernelIsRejected`, and Task 8, `unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt`.
3. **A missing kernel file** fails before any prompt with the cfg file name in the detail — Task 8, `unisiaBootmodeMissingKernelFailsBeforeAnyPrompt`.
4. **Esc or window-close on the `RemoveMod1` box** is a decline: nothing erased, notice shown, outcome Cancelled — Task 8, `unisiaBootmodeDeclinedMod1CancelsWithNotice`. (The box is modal and no worker is live, so `FlashDialog::closeEvent` never runs there.)
5. **An adapter that rejects 39063 baud / even parity at configure** fails attempt 1, skips `RemoveMod1` and attempt 2, and still shows the notice — covered by Task 8, `unisiaBootmodeKernelFailureSkipsMod1AndProgram` (any failed attempt-1 result).

---

# PR 7a — migrate the bootmode family

Branch: `feat/wave7-unisia-jecs-m32r-bootmode` (already created, carries the spec commit).

## File map (7a)

| File | Responsibility |
|---|---|
| `src/backend/flash/flash_executor.h` | `IKlineFlashTransport::enable_boot_mode_lines()` |
| `src/backend/flash/testing/scripted_kline_flash_transport.h` | scripted implementation of the new method |
| `src/platform/desktop/common/transport/desktop_kline_flash_transport.{h,cpp,_test.cpp}` | desktop binding + test |
| `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan{.cpp,_test.cpp}` | accept `_bootmode` Read |
| `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_types.h` | two plan structs |
| `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.{h,cpp,_test.cpp}` | builders + validator |
| `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.{h,cpp,_test.cpp}` | attempt 1 |
| `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.{h,cpp,_test.cpp}` | attempt 2 |
| `src/backend/flash/flash_types.h`, `flash_plan.cpp`, `flash_validation_test.cpp`, `testing/flash_printers.h`, `BUILD.bazel` | family registration |
| `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl` | targets |
| `src/platform/desktop/common/flash/flash_workflow.{h,cpp,_test.cpp}` | prompt kinds, route, workflow |
| `src/ui/desktop/flash/common/flash_dialog.{h,cpp,_test.cpp}` | prompt rendering |
| `src/ui/desktop/mainwindow.{h,cpp}`, `src/ui/desktop/BUILD.bazel` | drop legacy branches |
| `src/ui/desktop/flash/bootmode/` | deleted |
| `src/platform/desktop/common/flash/legacy/bootmode/`, `legacy/BUILD.bazel` | deleted / trimmed |
| `scripts/check-legacy-flash-drain.py`, `scripts/check-serial-compat-allowlist.py`, `src/platform/desktop/common/serial/BUILD.bazel` | ratchets shrink |
| `docs/…` | spec amendment, matrix, bench checklist, wave notes |

---

### Task 1: Amend the spec with the refinements found while planning

**Files:**
- Modify: `docs/superpowers/specs/2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md`

These refine the approved design without changing its behavior. Each is grounded in code read while planning.

- [ ] **Step 1: Edit the "Architecture (PR 7a) → Backend (portable)" Write bullets**

Replace the two-family bullet list and the builder bullet with:

```markdown
- `subaru_unisia_jecs_m32r_bootmode_types.h`: two `FlashFamily` values and
  their `FamilyPlan` alternatives, one per attempt, so each executor's
  `check_family()` rejects the other attempt's plan.
  - `SubaruUnisiaJecsM32rBootModeKernel`
  - `SubaruUnisiaJecsM32rBootModeProgram`

  Both set `family_requires_kernel_v = false`. The kernel attempt carries the
  zero-padded kernel as its **plan image**, as 6c-1 BDM does:
  `validate_and_build` requires every Write plan to carry an image, and the
  `_bootmode` cfg entries have no `<kernel_addr>`, so the shared
  `resolveKernel()` (which parses one) cannot load them.
- `subaru_unisia_jecs_m32r_bootmode_plan.{h,cpp}`:
  `build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(operation, protocol, mcu, kernel)`,
  `build_subaru_unisia_jecs_m32r_bootmode_program_plan(operation, protocol, mcu, image)`
  and `validate_…`. Both accept exactly the two `_bootmode` pairs and Write
  only.
  - Kernel plan: image = kernel zero-padded to a multiple of 128 bytes
    (`:312-315`); an empty kernel is rejected; transfer region
    `{0, padded size}`.
  - Program plan: image exactly `rom_size`; transfer region `{0, rom_size}`.
  - **Both** carry `ConfirmationSpec::Id::ApplyBootModeVoltages`: both
    executors drive programming-voltage lines, and presence means granted.
```

- [ ] **Step 2: Edit the Desktop section**

Add under the workflow bullet:

```markdown
  The kernel bytes come from a new `resolveKernelBytes()` beside
  `resolveKernel()`: the cfg `<kernel>` file, with no `<kernel_addr>`
  parsed. Both plans are built before `Begin`, so a missing kernel file or a
  wrong-size image fails before any prompt.
```

Replace the `flash_dialog.cpp` `RemoveProgrammingVoltage` sub-bullet and the "Close-to-cancel" paragraph with:

```markdown
  - `RemoveProgrammingVoltage` text moves into a static
    `FlashDialog::programmingVoltageNotice(const FlashPromptStep&)` so it is
    testable. With `power_off_advice=no`, a failed or cancelled write says
    "Press OK to exit and try again" (legacy bootmode's wording) instead of the
    don't-power-off advice; a successful one adds "Power cycle the ECU and
    request SSM Init to confirm the write."
- Close-to-cancel needs no dialog change. `finishCancelledAttempt()` submits
  one cancelled result and shows only prompts. The `RemoveMod1` box is modal
  and no worker is live while it shows, so closing it is a decline, handled by
  the workflow.
```

- [ ] **Step 3: Edit "Wire behavior → Negative replies"**

Replace the paragraph with:

```markdown
**Negative replies.** Legacy's success replies are `EF 42` and `EF 52`, a
status byte after `EF`; the documented error codes (`:346-353`) are read as
statuses in the same position. A valid `EF xx` frame in a failure detail is
annotated: `48` missing VPP, `5C` checksum error, `72` address error,
`8A` FENTRY bit not set, `42` erase started, `52` done; any other status is
"unknown". The bench checklist confirms the position.
```

- [ ] **Step 4: Edit Testing → Dialog**

Replace with:

```markdown
**Dialog** (`flash_dialog_test`): a workflow yielding two attempts with a
prompt between them runs both through `advance()`; `programmingVoltageNotice`
text for `power_off_advice` absent (6c-3 unchanged), `no` on success, and `no`
on failure.
```

- [ ] **Step 5: Add to "Deleted in 7a"**

```markdown
- The legacy package's `family_operation_sources` filegroup gains
  `allow_empty = True`: after `bootmode/` goes, its `*/*.cpp` pattern
  matches nothing until 7b deletes the package.
- The legacy library's `//src/algorithms/protocol/ssm/qt_compat` dependency;
  only the bootmode operation used it.
```

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md
git commit -m "docs(flash): refine the wave 7 bootmode design from planning findings

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 2: `IKlineFlashTransport::enable_boot_mode_lines()`

**Files:**
- Modify: `src/backend/flash/flash_executor.h:205-210`
- Modify: `src/backend/flash/testing/scripted_kline_flash_transport.h`
- Modify: `src/platform/desktop/common/transport/desktop_kline_flash_transport.h:48`, `.cpp` (after `enable_programming_voltage_line`)
- Test: `src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp:135-148`, `:483-486`

**Interfaces:**
- Produces: `virtual Status IKlineFlashTransport::enable_boot_mode_lines() = 0;`
  `ScriptedKlineFlashTransport::ControlLineAction::EnableBootModeLines`,
  `ScriptedKlineFlashTransport::Operation::EnableBootModeLines`,
  `Status ScriptedKlineFlashTransport::enable_boot_mode_lines_result_`.

The fake serial maps "enabled" to `0` and "disabled" to `1` (existing test: disable → `set_lec_lines(1, 1)`, programming → `(0, 1)`), so boot mode is `set_lec_lines(0, 0)`.

- [ ] **Step 1: Write the failing adapter tests**

In `lecControlOperationsForwardToSerialBackend`, extend the sequence:

```cpp
        EXPECT_CALL(serial.fake(), set_lec_lines(0, 1)).WillOnce(::testing::Return(STATUS_SUCCESS));
        EXPECT_CALL(serial.fake(), set_lec_lines(0, 0)).WillOnce(::testing::Return(STATUS_SUCCESS));

        DesktopKlineFlashTransport transport(serial.release());

        QVERIFY(transport.disable_lec_lines().has_value());
        QVERIFY(transport.pulse_lec_2_line(200ms).has_value());
        QVERIFY(transport.enable_programming_voltage_line().has_value());
        // Legacy bootmode execute() :66 -- VPP on LEC1 and MOD1 on LEC2.
        QVERIFY(transport.enable_boot_mode_lines().has_value());
```

After the `programmingLineResult` block in the closed-transport test (`:483-486`):

```cpp
        const auto bootModeLinesResult = transport.enable_boot_mode_lines();
        QVERIFY(!bootModeLinesResult.has_value());
        QCOMPARE(bootModeLinesResult.error().kind, ErrorKind::Disconnected);
```

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/platform/desktop/common/transport:all`
Expected: compile error, `no member named 'enable_boot_mode_lines'`.

- [ ] **Step 3: Add the port method**

In `flash_executor.h`, after `enable_programming_voltage_line()`:

```cpp
    // Wave 7. Programming voltage on LEC1 and MOD1 on LEC2 together: the M32R
    // boot-mode entry state the Unisia Jecs bootmode kernel upload needs.
    virtual Status enable_boot_mode_lines() = 0;
```

In `desktop_kline_flash_transport.h` after line 48:

```cpp
    Status enable_boot_mode_lines() override;
```

In `desktop_kline_flash_transport.cpp`, after `enable_programming_voltage_line()`:

```cpp
Status DesktopKlineFlashTransport::enable_boot_mode_lines()
{
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "enable_boot_mode_lines() called after close()");
    }
    try
    {
        // Legacy flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp:66.
        if (serial_->set_lec_lines(serial_->get_requestToSendEnabled(), serial_->get_dataTerminalEnabled()) != 0)
        {
            return fail(ErrorKind::Internal, "set_lec_lines boot mode state failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "enable_boot_mode_lines exception");
    }
}
```

In `scripted_kline_flash_transport.h`, add `EnableBootModeLines` as the last enumerator of both `ControlLineAction` and `Operation`, then after `enable_programming_voltage_line()`:

```cpp
    Status enable_boot_mode_lines() override
    {
        control_line_trace_.push_back(ControlLineAction::EnableBootModeLines);
        operation_trace_.push_back(Operation::EnableBootModeLines);
        return enable_boot_mode_lines_result_;
    }
```

and beside `enable_programming_voltage_line_result_`:

```cpp
    Status enable_boot_mode_lines_result_;
```

- [ ] **Step 4: Run the tests**

Run: `bazel test --config=release //src/platform/desktop/common/transport:all //src/backend/flash/...`
Expected: PASS. (`subaru_denso_mc68hc16y5_02_executor_test` and `subaru_denso_sh7055_02_executor_test` use `ControlLineAction` values by name; appending an enumerator does not affect them.)

- [ ] **Step 5: Commit**

```bash
git add src/backend/flash/flash_executor.h src/backend/flash/testing/scripted_kline_flash_transport.h \
  src/platform/desktop/common/transport/desktop_kline_flash_transport.h \
  src/platform/desktop/common/transport/desktop_kline_flash_transport.cpp \
  src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp
git commit -m "feat(flash): add enable_boot_mode_lines to the K-Line flash transport

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 3: Accept `_bootmode` Read in the 6c-3 plan

**Files:**
- Modify: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.cpp:25-32`, `.h` comment
- Test: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan_test.cpp:108-123`, `subaru_unisia_jecs_m32r_kline_executor_test.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_{20,30}_bootmode", "M32R_{128,256}KB", std::nullopt, …)` succeeds; Write/TestWrite on those names fail `Unsupported`.

- [ ] **Step 1: Write the failing plan tests**

In `RejectsEveryOtherIdentity`, remove the `{"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB"}` row and add `{"sub_ecu_unisia_jecs_20_bootmode", "M32R_256KB"}` (crossed) and `{"sub_ecu_unisia_jecs_40_bootmode", "M32R_384KB"}` (nonexistent). Then add:

```cpp
// Wave 7. Bootmode Read is byte-identical to this family's Read
// (flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp:112-275); bootmode
// Write belongs to the bootmode family.
TEST(SubaruUnisiaJecsM32rKlinePlan, AcceptsBootmodeProtocolsForReadOnly)
{
    struct Bootmode
    {
        std::string_view protocol;
        std::string_view mcu;
        std::uint32_t rom_size;
    };
    for (const Bootmode& variant : std::to_array<Bootmode>({
             {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
             {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
         }))
    {
        const auto read = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, variant.protocol,
                                                                   variant.mcu, std::nullopt, false);
        ASSERT_THAT(read, IsOk()) << variant.protocol;
        EXPECT_EQ(read->transfer_region(), (MemoryRegion{0x100000, variant.rom_size}));
        EXPECT_TRUE(read->confirmations().empty());
        EXPECT_THAT(validate_subaru_unisia_jecs_m32r_kline_plan(*read), IsOk());

        for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
        {
            EXPECT_THAT(build_subaru_unisia_jecs_m32r_kline_plan(operation, variant.protocol, variant.mcu,
                                                                 bytes::Bytes(variant.rom_size, 0x00), false),
                        IsErr(ErrorKind::Unsupported))
                << variant.protocol;
        }
    }
}
```

In `subaru_unisia_jecs_m32r_kline_executor_test.cpp`, add:

```cpp
TEST(SubaruUnisiaJecsM32rKlineExecutor, ReadsABootmodeProtocolWithTheSameWireSequence)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    script_pages(transport, 0x40000 / 0x80);
    RunContext context;
    const auto result = run(read_plan("sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB"), transport, context);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->read_bytes->size(), 0x40000U);
    EXPECT_EQ(result->rom_id, std::optional<std::string>("123456789A_"));
    EXPECT_TRUE(transport.scriptConsumed());
}
```

- [ ] **Step 2: Run to see them fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_plan_test //src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_executor_test`
Expected: FAIL — bootmode Read returns `InvalidConfig`.

- [ ] **Step 3: Add the variants**

In `kVariants`:

```cpp
// protocols.cfg: _20 and _30 are read/write; _40 and _70 are read-only.
// test_write is "no" for all four. The two _bootmode names are Read-only
// here: their Read is this family's wire sequence (wave 7), and their Write
// is the bootmode family's two-attempt kernel upload and program.
constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000, true},
    {"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000, true},
    {"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000, false},
    {"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000, false},
    {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000, false},
    {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000, false},
});
```

Update the header comment in `subaru_unisia_jecs_m32r_kline_plan.h`:

```cpp
// Exact protocol/MCU pairs sub_ecu_unisia_jecs_{20,30,40,70}, plus Read on
// sub_ecu_unisia_jecs_{20,30}_bootmode; _40, _70 and both _bootmode names are
// read-only here. A Write plan carries ApplyProgrammingVoltage unless
// `adapter_supplies_programming_voltage`; the desktop workflow collects that
// confirmation before the executor starts.
```

If the existing `kVariants`-driven tests iterate every variant and assert Write acceptance only when `writable`, they keep passing; if any asserts `kVariants.size() == 4`, change it to 6.

- [ ] **Step 4: Run the tests**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.cpp \
  src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan_test.cpp src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor_test.cpp
git commit -m "feat(flash): read Unisia Jecs M32R bootmode protocols through the K-Line family

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 4: Register the two bootmode families

**Files:**
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_types.h`
- Modify: `src/backend/flash/flash_types.h` (include, `FlashFamily`, `ConfirmationSpec::Id`, `FamilyPlan`, `FamilyTraits`, `family_requires_kernel_v`)
- Modify: `src/backend/flash/flash_plan.cpp:62`, `src/backend/flash/testing/flash_printers.h:115`, `src/backend/flash/BUILD.bazel:40`, `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl:126`
- Test: `src/backend/flash/flash_validation_test.cpp:72-226`

**Interfaces:**
- Produces:
  ```cpp
  struct SubaruUnisiaJecsM32rBootModeKernelPlan { int initial_baud; std::uint8_t tester_id; std::uint8_t target_id; };
  struct SubaruUnisiaJecsM32rBootModeProgramPlan { int initial_baud; std::uint8_t tester_id; std::uint8_t target_id; };
  FlashFamily::SubaruUnisiaJecsM32rBootModeKernel, FlashFamily::SubaruUnisiaJecsM32rBootModeProgram
  ConfirmationSpec::Id::ApplyBootModeVoltages
  experimental_family_id(): "SubaruUnisiaJecsM32rBootModeKernel" / "SubaruUnisiaJecsM32rBootModeProgram"
  ```

- [ ] **Step 1: Write the failing validation test**

In `flash_validation_test.cpp`, change both `28`s to `30` and append to `family_cases()`:

```cpp
        {FlashFamily::SubaruUnisiaJecsM32rBootModeKernel, TransportKind::Kline,
         SubaruUnisiaJecsM32rBootModeKernelPlan{.initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10},
         "SubaruUnisiaJecsM32rBootModeKernel"},
        {FlashFamily::SubaruUnisiaJecsM32rBootModeProgram, TransportKind::Kline,
         SubaruUnisiaJecsM32rBootModeProgramPlan{.initial_baud = 19200, .tester_id = 0xf0, .target_id = 0x10},
         "SubaruUnisiaJecsM32rBootModeProgram"},
```

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/backend/flash:flash_validation_test`
Expected: compile error on the unknown types.

- [ ] **Step 3: Add the types header**

`src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_types.h`:

```cpp
#pragma once

#include <cstdint>

namespace fastecu::flash
{
// Step 5 tail, wave 7. Bootmode Write is two attempts: the kernel upload into
// the M32R boot ROM, then erase and program through that kernel. Each attempt
// is its own family so each executor's check_family() rejects the other's
// plan. The ROM and kernel sizes come from the plan's transfer region.
struct SubaruUnisiaJecsM32rBootModeKernelPlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

struct SubaruUnisiaJecsM32rBootModeProgramPlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};
} // namespace fastecu::flash
```

- [ ] **Step 4: Register in `flash_types.h`**

Add `#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_types.h"` after the `subaru_unisia_jecs_m32r_kline_types.h` include. Append to `FlashFamily`:

```cpp
    // Step 5 tail, wave 7.
    SubaruUnisiaJecsM32rBootModeKernel,
    SubaruUnisiaJecsM32rBootModeProgram,
```

Append to `ConfirmationSpec::Id` after `ApplyProgrammingVoltage`:

```cpp
        // Step 5 tail, wave 7. Same contract: the operator confirmed, before
        // the executor started, that VPP and MOD1 are connected for M32R
        // boot mode.
        ApplyBootModeVoltages,
```

Append `SubaruUnisiaJecsM32rBootModeKernelPlan, SubaruUnisiaJecsM32rBootModeProgramPlan` to the end of `FamilyPlan`. After the `FamilyTraits<SubaruUnisiaJecsM32rKlinePlan>` specialization:

```cpp
template <> struct FamilyTraits<SubaruUnisiaJecsM32rBootModeKernelPlan>
{
    static constexpr FlashFamily family = FlashFamily::SubaruUnisiaJecsM32rBootModeKernel;
    static constexpr TransportKind transport = TransportKind::Kline;
};

template <> struct FamilyTraits<SubaruUnisiaJecsM32rBootModeProgramPlan>
{
    static constexpr FlashFamily family = FlashFamily::SubaruUnisiaJecsM32rBootModeProgram;
    static constexpr TransportKind transport = TransportKind::Kline;
};
```

At the end of the `family_requires_kernel_v` list:

```cpp
// Step 5 tail, wave 7. The kernel attempt carries the cfg kernel as its plan
// image, as 6c-1 BDM does: the _bootmode cfg entries declare no kernel_addr,
// and the M32R boot ROM places the kernel itself. The program attempt uploads
// nothing.
template <> inline constexpr bool family_requires_kernel_v<SubaruUnisiaJecsM32rBootModeKernelPlan> = false;
template <> inline constexpr bool family_requires_kernel_v<SubaruUnisiaJecsM32rBootModeProgramPlan> = false;
```

- [ ] **Step 5: Register ids, printers, BUILD, portable list**

`flash_plan.cpp`, in `experimental_family_id()` after the `SubaruUnisiaJecsM32rKline` case:

```cpp
    case FlashFamily::SubaruUnisiaJecsM32rBootModeKernel:
        return "SubaruUnisiaJecsM32rBootModeKernel";
    case FlashFamily::SubaruUnisiaJecsM32rBootModeProgram:
        return "SubaruUnisiaJecsM32rBootModeProgram";
```

`flash_printers.h`, after the `SubaruUnisiaJecsM32rKline` case:

```cpp
    case FlashFamily::SubaruUnisiaJecsM32rBootModeKernel:
        *os << "SubaruUnisiaJecsM32rBootModeKernel";
        return;
    case FlashFamily::SubaruUnisiaJecsM32rBootModeProgram:
        *os << "SubaruUnisiaJecsM32rBootModeProgram";
        return;
```

`src/backend/flash/BUILD.bazel`, in the `flash_types` deps after `subaru_unisia_jecs_m32r_kline_types`:

```python
        "//src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_types",
```

`src/backend/flash/ecu/BUILD.bazel`, after the `subaru_unisia_jecs_m32r_kline_executor_test` rule:

```python
cc_library(
    name = "subaru_unisia_jecs_m32r_bootmode_types",
    hdrs = ["subaru_unisia_jecs_m32r_bootmode_types.h"],
)
```

`bazel/portable_targets.bzl`, after `"subaru_unisia_jecs_m32r_kline_executor",`:

```python
        "subaru_unisia_jecs_m32r_bootmode_types",
```

- [ ] **Step 6: Run the tests**

Run: `bazel test --config=release //src/backend/flash/... && bazel build --config=release //:portable_closure`
Expected: PASS. A `-Wswitch` error names any other exhaustive `FlashFamily` switch; add the two cases there the same way.

- [ ] **Step 7: Commit**

```bash
git add src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_types.h src/backend/flash/flash_types.h \
  src/backend/flash/flash_plan.cpp src/backend/flash/testing/flash_printers.h src/backend/flash/BUILD.bazel \
  src/backend/flash/ecu/BUILD.bazel bazel/portable_targets.bzl src/backend/flash/flash_validation_test.cpp
git commit -m "feat(flash): register the Unisia Jecs M32R bootmode kernel and program families

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 5: Bootmode plan builders and validator

**Files:**
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: Task 4 types, `ConfirmationSpec::Id::ApplyBootModeVoltages`.
- Produces:
  ```cpp
  Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation operation, std::string_view protocol_name, std::string_view mcu_type, bytes::Bytes kernel);
  Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation operation, std::string_view protocol_name, std::string_view mcu_type, std::optional<bytes::Bytes> image);
  Status validate_subaru_unisia_jecs_m32r_bootmode_plan(const FlashPlan& plan); // either family
  ```

- [ ] **Step 1: Write the failing tests**

`subaru_unisia_jecs_m32r_bootmode_plan_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

struct Variant
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
};

constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
    {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
});

bool carries_only_voltage_confirmation(const FlashPlan& plan)
{
    return plan.confirmations().size() == 1 &&
           plan.confirmations()[0].id == ConfirmationSpec::Id::ApplyBootModeVoltages;
}

TEST(SubaruUnisiaJecsM32rBootModePlan, KernelPlanPadsToWholeChunks)
{
    for (const Variant& variant : kVariants)
    {
        const auto plan = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(
            FlashOperation::Write, variant.protocol, variant.mcu, bytes::Bytes(200, 0x5a));
        ASSERT_THAT(plan, IsOk()) << variant.protocol;
        bytes::Bytes expected(200, 0x5a);
        expected.resize(256, 0x00); // upload_kernel() :312-315
        EXPECT_EQ(plan->image(), std::optional<bytes::Bytes>(expected));
        EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, 256}));
        EXPECT_EQ(plan->family(), FlashFamily::SubaruUnisiaJecsM32rBootModeKernel);
        EXPECT_FALSE(plan->kernel().has_value());
        EXPECT_TRUE(carries_only_voltage_confirmation(*plan));
        EXPECT_THAT(validate_subaru_unisia_jecs_m32r_bootmode_plan(*plan), IsOk());
    }
}

TEST(SubaruUnisiaJecsM32rBootModePlan, KernelAlreadyAlignedIsNotPadded)
{
    const auto plan = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(
        FlashOperation::Write, "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", bytes::Bytes(0x100, 0x11));
    ASSERT_THAT(plan, IsOk());
    EXPECT_EQ(plan->image()->size(), 0x100U);
}

TEST(SubaruUnisiaJecsM32rBootModePlan, EmptyKernelIsRejected)
{
    EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation::Write,
                                                                   "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", {}),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruUnisiaJecsM32rBootModePlan, ProgramPlanTakesExactlyTheRomSize)
{
    for (const Variant& variant : kVariants)
    {
        const auto plan = build_subaru_unisia_jecs_m32r_bootmode_program_plan(
            FlashOperation::Write, variant.protocol, variant.mcu, bytes::Bytes(variant.rom_size, 0x5a));
        ASSERT_THAT(plan, IsOk()) << variant.protocol;
        EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, variant.rom_size}));
        EXPECT_EQ(plan->family(), FlashFamily::SubaruUnisiaJecsM32rBootModeProgram);
        EXPECT_TRUE(carries_only_voltage_confirmation(*plan));
        EXPECT_THAT(validate_subaru_unisia_jecs_m32r_bootmode_plan(*plan), IsOk());

        for (const std::uint32_t size : {variant.rom_size - 1, variant.rom_size + 1, variant.rom_size - 0x40})
        {
            EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation::Write, variant.protocol,
                                                                            variant.mcu, bytes::Bytes(size, 0x5a)),
                        IsErr(ErrorKind::InvalidConfig))
                << variant.protocol << " size " << size;
        }
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation::Write, variant.protocol,
                                                                        variant.mcu, std::nullopt),
                    IsErr(ErrorKind::InvalidConfig));
    }
}

TEST(SubaruUnisiaJecsM32rBootModePlan, RejectsReadAndTestWrite)
{
    for (const FlashOperation operation : {FlashOperation::Read, FlashOperation::TestWrite})
    {
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(operation, "sub_ecu_unisia_jecs_20_bootmode",
                                                                       "M32R_128KB", bytes::Bytes(1, 0x00)),
                    IsErr(ErrorKind::Unsupported));
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_program_plan(operation, "sub_ecu_unisia_jecs_20_bootmode",
                                                                        "M32R_128KB", bytes::Bytes(0x20000, 0x00)),
                    IsErr(ErrorKind::Unsupported));
    }
}

TEST(SubaruUnisiaJecsM32rBootModePlan, RejectsEveryOtherIdentity)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_unisia_jecs_20_bootmode", "M32R_256KB"},
             {"sub_ecu_unisia_jecs_20", "M32R_128KB"},
             {"sub_ecu_unisia_jecs_20_bootmodex", "M32R_128KB"},
             {"sub_ecu_unisia_jecs_40_bootmode", "M32R_384KB"},
         }))
    {
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation::Write, protocol, mcu,
                                                                       bytes::Bytes(1, 0x00)),
                    IsErr(ErrorKind::InvalidConfig))
            << protocol << " / " << mcu;
    }
}

// Presence means granted: a plan assembled without the confirmation (never
// through the builders) must not validate, so no executor raises the lines.
TEST(SubaruUnisiaJecsM32rBootModePlan, ValidatorRejectsAPlanWithoutTheVoltageConfirmation)
{
    auto plan = validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = FlashFamily::SubaruUnisiaJecsM32rBootModeKernel,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_unisia_jecs_20_bootmode",
        .mcu_name = "M32R_128KB",
        .transfer_region = {0, 0x80},
        .erase_regions = {},
        .image = bytes::Bytes(0x80, 0x00),
        .kernel = std::nullopt,
        .family_plan = SubaruUnisiaJecsM32rBootModeKernelPlan{.initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10},
        .confirmations = {},
    });
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(validate_subaru_unisia_jecs_m32r_bootmode_plan(*plan), IsErr(ErrorKind::InvalidConfig));
}
} // namespace
} // namespace fastecu::flash
```

Add the BUILD rules after `subaru_unisia_jecs_m32r_bootmode_types`:

```python
cc_library(
    name = "subaru_unisia_jecs_m32r_bootmode_plan",
    srcs = ["subaru_unisia_jecs_m32r_bootmode_plan.cpp"],
    hdrs = ["subaru_unisia_jecs_m32r_bootmode_plan.h"],
    deps = [
        ":subaru_unisia_jecs_m32r_bootmode_types",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/flash:flash_plan",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_unisia_jecs_m32r_bootmode_plan_test",
    srcs = ["subaru_unisia_jecs_m32r_bootmode_plan_test.cpp"],
    deps = [
        ":subaru_unisia_jecs_m32r_bootmode_plan",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

and add `"subaru_unisia_jecs_m32r_bootmode_plan",` to `bazel/portable_targets.bzl` after the types entry.

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_plan_test`
Expected: FAIL — missing header.

- [ ] **Step 3: Write the header**

`subaru_unisia_jecs_m32r_bootmode_plan.h`:

```cpp
#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
// Wave 7. Exact protocol/MCU pairs sub_ecu_unisia_jecs_20_bootmode /
// M32R_128KB and sub_ecu_unisia_jecs_30_bootmode / M32R_256KB, Write only
// (Read goes through the 6c-3 K-Line family). Write is two plans run as two
// attempts: the kernel upload, then erase and program. Both carry
// ApplyBootModeVoltages, collected by the desktop workflow before either runs.
Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation operation,
                                                                     std::string_view protocol_name,
                                                                     std::string_view mcu_type, bytes::Bytes kernel);
Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation operation,
                                                                      std::string_view protocol_name,
                                                                      std::string_view mcu_type,
                                                                      std::optional<bytes::Bytes> image);
// Validates a plan of either bootmode family.
Status validate_subaru_unisia_jecs_m32r_bootmode_plan(const FlashPlan& plan);
} // namespace fastecu::flash
```

- [ ] **Step 4: Write the implementation**

`subaru_unisia_jecs_m32r_bootmode_plan.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
struct Variant
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
};

// protocols.cfg: both are read=yes, test_write=no, write=yes; Read is served
// by the 6c-3 K-Line family.
constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
    {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
});
constexpr std::uint32_t kChunk = 0x80; // upload_kernel() :312-315, write_mem() :469
// execute() :55-58 and write_mem() :362-364.
constexpr SubaruUnisiaJecsM32rBootModeKernelPlan kKernelWire{
    .initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10};
constexpr SubaruUnisiaJecsM32rBootModeProgramPlan kProgramWire{
    .initial_baud = 19200, .tester_id = 0xf0, .target_id = 0x10};

Result<Variant> find_variant(std::string_view protocol, std::string_view mcu)
{
    const auto variant = std::ranges::find(kVariants, protocol, &Variant::protocol);
    if (variant == kVariants.end() || variant->mcu != mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Unisia Jecs M32R bootmode protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const flashdev_t *device = find_flash_device(mcu);
    if (device == nullptr || device->romsize != variant->rom_size)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode memory map is invalid");
    }
    return *variant;
}

Status check_operation(FlashOperation operation)
{
    if (operation != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported, "Unisia Jecs M32R bootmode supports only Write here");
    }
    return {};
}

std::vector<ConfirmationSpec> voltage_confirmation()
{
    return {ConfirmationSpec{ConfirmationSpec::Id::ApplyBootModeVoltages, {}}};
}

bool has_only_voltage_confirmation(const FlashPlan& plan)
{
    const auto& confirmations = plan.confirmations();
    return confirmations.size() == 1 && confirmations[0].id == ConfirmationSpec::Id::ApplyBootModeVoltages;
}

template <typename Wire> bool wire_matches(const Wire& wire, const Wire& expected)
{
    return wire.initial_baud == expected.initial_baud && wire.tester_id == expected.tester_id &&
           wire.target_id == expected.target_id;
}

Status validate_kernel(const FlashPlan& plan)
{
    const auto *wire = std::get_if<SubaruUnisiaJecsM32rBootModeKernelPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_matches(*wire, kKernelWire))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode kernel wire parameters are invalid");
    }
    if (!plan.image().has_value() || plan.image()->empty() || plan.image()->size() % kChunk != 0 ||
        plan.transfer_region() != MemoryRegion{0, static_cast<std::uint32_t>(plan.image()->size())})
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode kernel image shape is invalid");
    }
    return {};
}

Status validate_program(const FlashPlan& plan, const Variant& variant)
{
    const auto *wire = std::get_if<SubaruUnisiaJecsM32rBootModeProgramPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_matches(*wire, kProgramWire))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode program wire parameters are invalid");
    }
    if (!plan.image().has_value() || plan.image()->size() != variant.rom_size ||
        plan.transfer_region() != MemoryRegion{0, variant.rom_size})
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode write image must be exactly the ROM size");
    }
    return {};
}

Result<FlashPlan> build(FlashFamily family, const Variant& variant, bytes::Bytes image, FamilyPlan wire)
{
    const auto length = static_cast<std::uint32_t>(image.size());
    auto plan = validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = family,
        .transport = TransportKind::Kline,
        .target_id = std::string(variant.protocol),
        .mcu_name = std::string(variant.mcu),
        .transfer_region = {0, length},
        .erase_regions = {},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = std::move(wire),
        .confirmations = voltage_confirmation(),
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace

Status validate_subaru_unisia_jecs_m32r_bootmode_plan(const FlashPlan& plan)
{
    const bool kernel = plan.family() == FlashFamily::SubaruUnisiaJecsM32rBootModeKernel;
    if ((!kernel && plan.family() != FlashFamily::SubaruUnisiaJecsM32rBootModeProgram) ||
        plan.transport() != TransportKind::Kline)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Unisia Jecs M32R bootmode");
    }
    const auto variant = find_variant(plan.target_id(), plan.mcu_name());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status operation = check_operation(plan.operation()); !operation.has_value())
    {
        return operation;
    }
    if (!plan.erase_regions().empty() || plan.kernel().has_value() || !has_only_voltage_confirmation(plan))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode plan shape is invalid");
    }
    return kernel ? validate_kernel(plan) : validate_program(plan, *variant);
}

Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation operation,
                                                                     std::string_view protocol_name,
                                                                     std::string_view mcu_type, bytes::Bytes kernel)
{
    const auto variant = find_variant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status checked = check_operation(operation); !checked.has_value())
    {
        return std::unexpected(checked.error());
    }
    if (kernel.empty())
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode kernel file is empty");
    }
    // upload_kernel() :312-315: zero-pad to whole 128-byte chunks.
    kernel.resize((kernel.size() + kChunk - 1) / kChunk * kChunk, 0x00);
    return build(FlashFamily::SubaruUnisiaJecsM32rBootModeKernel, *variant, std::move(kernel), kKernelWire);
}

Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation operation,
                                                                      std::string_view protocol_name,
                                                                      std::string_view mcu_type,
                                                                      std::optional<bytes::Bytes> image)
{
    const auto variant = find_variant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status checked = check_operation(operation); !checked.has_value())
    {
        return std::unexpected(checked.error());
    }
    if (!image.has_value() || image->size() != variant->rom_size)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} write image must be exactly {} bytes, not {}", variant->protocol,
                                variant->rom_size, image.has_value() ? image->size() : 0));
    }
    return build(FlashFamily::SubaruUnisiaJecsM32rBootModeProgram, *variant, std::move(*image), kProgramWire);
}
} // namespace fastecu::flash
```

- [ ] **Step 5: Run the tests**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_plan_test && bazel build --config=release //:portable_closure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.cpp \
  src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan_test.cpp src/backend/flash/ecu/BUILD.bazel bazel/portable_targets.bzl
git commit -m "feat(flash): plan Unisia Jecs M32R bootmode kernel upload and program attempts

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 6: Kernel-upload executor (attempt 1)

**Files:**
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: Task 2 `enable_boot_mode_lines()`, Task 5 builders/validator.
- Produces: `class SubaruUnisiaJecsM32rBootModeKernelExecutor final : public IKlineFlashExecutor` with `transport_setup`, `before_transport_configure`, `execute`.

- [ ] **Step 1: Write the failing tests**

`subaru_unisia_jecs_m32r_bootmode_kernel_executor_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using Line = ScriptedKlineFlashTransport::ControlLineAction;

// 200 bytes: two 128-byte chunks once padded.
bytes::Bytes kernel_file()
{
    bytes::Bytes kernel(200);
    for (std::size_t i = 0; i < kernel.size(); ++i)
    {
        kernel[i] = static_cast<bytes::Byte>(i + 1);
    }
    return kernel;
}

FlashPlan kernel_plan()
{
    auto plan = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation::Write,
                                                                   "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB",
                                                                   kernel_file());
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

bytes::Bytes chunk(const FlashPlan& plan, std::size_t index)
{
    const auto begin = plan.image()->begin() + static_cast<std::ptrdiff_t>(index * 0x80);
    return bytes::Bytes(begin, begin + 0x80);
}

struct RunContext
{
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
};

Result<FlashExecutionResult> run(const FlashPlan& plan, ScriptedKlineFlashTransport& transport, RunContext& context)
{
    return SubaruUnisiaJecsM32rBootModeKernelExecutor{}.execute(plan, transport, context.clock, context.cancellation,
                                                                context.events);
}

void script_upload(ScriptedKlineFlashTransport& transport, const FlashPlan& plan)
{
    auto section = transport.section("kernel chunks");
    transport.expectWrite(chunk(plan, 0));
    transport.expectWrite(chunk(plan, 1));
    transport.queue_no_frame(); // the discarded settle read
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, TransportSetupIs39063BaudEvenParity)
{
    const auto setup = SubaruUnisiaJecsM32rBootModeKernelExecutor{}.transport_setup(kernel_plan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 39063);             // execute() :58
    EXPECT_EQ(setup->parity, KlineParity::Even); // execute() :57
    EXPECT_FALSE(setup->iso14230);             // execute() :53
    EXPECT_EQ(setup->tester_id, 0xf0);         // execute() :59
    EXPECT_EQ(setup->target_id, 0x10);         // execute() :60
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, BeforeConfigureResetsThenClearsTheHeader)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(
        SubaruUnisiaJecsM32rBootModeKernelExecutor{}.before_transport_configure(transport, clock, cancellation),
        IsOk());
    EXPECT_EQ(transport.lifecycle_calls_, std::vector<std::string>{"reset_connection"}); // execute() :52
    EXPECT_EQ(transport.header_mode_calls_, std::vector<bool>{false});
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, UploadsEveryChunkUnderBootModeLines)
{
    const FlashPlan plan = kernel_plan();
    ScriptedKlineFlashTransport transport;
    script_upload(transport, plan);
    RunContext context;
    ASSERT_THAT(run(plan, transport, context), IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_, (std::vector{Line::EnableBootModeLines, Line::DisableLecLines}));
    EXPECT_EQ(transport.read_timeouts_, std::vector<std::chrono::milliseconds>{200ms}); // :339
    EXPECT_EQ(context.clock.elapsed(), 500ms);                                           // :338
    EXPECT_EQ(context.events.progress_calls, (std::vector<std::pair<int, int>>{{1, 2}, {2, 2}}));
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, SettleReplyIsLoggedAndIgnored)
{
    const FlashPlan plan = kernel_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(chunk(plan, 0));
    transport.expectWrite(chunk(plan, 1));
    transport.queueRead(bytes::Bytes{0x55, 0xaa});
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsOk());
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, APlanWithoutTheConfirmationTouchesNothing)
{
    auto plan = validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = FlashFamily::SubaruUnisiaJecsM32rBootModeKernel,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_unisia_jecs_20_bootmode",
        .mcu_name = "M32R_128KB",
        .transfer_region = {0, 0x80},
        .erase_regions = {},
        .image = bytes::Bytes(0x80, 0x00),
        .kernel = std::nullopt,
        .family_plan = SubaruUnisiaJecsM32rBootModeKernelPlan{.initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10},
        .confirmations = {},
    });
    ASSERT_THAT(plan, IsOk());
    ScriptedKlineFlashTransport transport;
    RunContext context;
    EXPECT_THAT(run(*plan, transport, context), IsErr(ErrorKind::InvalidConfig));
    EXPECT_TRUE(transport.control_line_trace_.empty());
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, RejectsAProgramPlan)
{
    auto plan = build_subaru_unisia_jecs_m32r_bootmode_program_plan(
        FlashOperation::Write, "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", bytes::Bytes(0x20000, 0x00));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeKernelExecutor{}.transport_setup(*plan), IsErr(ErrorKind::InvalidConfig));
}

// Check 1 precedes the lines, checks 2 and 3 precede each chunk.
TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, CancellationAtEachCheckpointDropsTheLines)
{
    for (const std::size_t check : {1U, 2U, 3U})
    {
        const FlashPlan plan = kernel_plan();
        ScriptedKlineFlashTransport transport;
        script_upload(transport, plan);
        RunContext context;
        context.cancellation.cancel_on_check(check);
        EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Cancelled)) << "check " << check;
        ASSERT_FALSE(transport.control_line_trace_.empty());
        EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines) << "check " << check;
        // Check 1 stops before any chunk; check N >= 2 stops before chunk N-2.
        EXPECT_EQ(transport.writesConsumed(), check >= 2 ? check - 2 : 0U) << "check " << check;
    }
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, LineFailureStillDropsTheLinesAndKeepsItsError)
{
    const FlashPlan plan = kernel_plan();
    ScriptedKlineFlashTransport transport;
    transport.enable_boot_mode_lines_result_ = fail(ErrorKind::Internal, "lines");
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Internal));
    EXPECT_EQ(transport.control_line_trace_, (std::vector{Line::EnableBootModeLines, Line::DisableLecLines}));
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, CleanupFailureFailsAnOtherwiseGoodUpload)
{
    const FlashPlan plan = kernel_plan();
    ScriptedKlineFlashTransport transport;
    script_upload(transport, plan);
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Disconnected));
}
} // namespace
} // namespace fastecu::flash
```

BUILD rules:

```python
cc_library(
    name = "subaru_unisia_jecs_m32r_bootmode_kernel_executor",
    srcs = ["subaru_unisia_jecs_m32r_bootmode_kernel_executor.cpp"],
    hdrs = ["subaru_unisia_jecs_m32r_bootmode_kernel_executor.h"],
    deps = [
        ":subaru_unisia_jecs_m32r_bootmode_plan",
        "//src/algorithms/protocol",
        "//src/backend/flash:flash_executor",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_unisia_jecs_m32r_bootmode_kernel_executor_test",
    srcs = ["subaru_unisia_jecs_m32r_bootmode_kernel_executor_test.cpp"],
    deps = [
        ":subaru_unisia_jecs_m32r_bootmode_kernel_executor",
        ":subaru_unisia_jecs_m32r_bootmode_plan",
        "//src/backend/flash:flash_validation",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:recording_event_sink",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

Add `"subaru_unisia_jecs_m32r_bootmode_kernel_executor",` to `bazel/portable_targets.bzl`.

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_kernel_executor_test`
Expected: FAIL — missing header.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// Wave 7, attempt 1 of a bootmode Write: uploads the padded kernel into the
// M32R boot ROM with VPP and MOD1 raised.
class SubaruUnisiaJecsM32rBootModeKernelExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Status before_transport_configure(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
```

- [ ] **Step 4: Write the implementation**

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <string_view>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

// Legacy flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.{h,cpp}.
constexpr std::size_t kChunk = 0x80;  // upload_kernel() :320-330
constexpr auto kSettle = 500ms;       // upload_kernel() :338
constexpr auto kSettleRead = 200ms;   // serial_read_short_timeout, :339

Status cancelled_if_requested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::format("cancelled {}", where));
    }
    return {};
}

Status upload(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
              const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status cancelled = cancelled_if_requested(cancellation, "before boot mode lines"); !cancelled.has_value())
    {
        return cancelled;
    }
    // execute() :64-67.
    events.log(LogLevel::Info, "Set programming voltage +12v to Line End Check 1 and MOD1 to Line End Check 2");
    if (Status raised = transport.enable_boot_mode_lines(); !raised.has_value())
    {
        return raised;
    }

    // upload_kernel() :318-335: unframed 128-byte chunks, echo-checked.
    events.log(LogLevel::Info, "Uploading kernel, please wait...");
    const bytes::Bytes& kernel = *plan.image();
    const auto chunks = static_cast<int>(kernel.size() / kChunk);
    for (int index = 0; index < chunks; ++index)
    {
        if (Status cancelled = cancelled_if_requested(cancellation, "during kernel upload"); !cancelled.has_value())
        {
            return cancelled;
        }
        const bytes::ByteView data =
            bytes::ByteView(kernel).subspan(static_cast<std::size_t>(index) * kChunk, kChunk);
        auto written = transport.write(data);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (*written != data.size())
        {
            return fail(ErrorKind::Disconnected, "short K-Line write during kernel upload");
        }
        events.progress(index + 1, chunks);
    }

    // upload_kernel() :338-340: legacy read and discarded whatever followed.
    // Nothing here proves the kernel runs; attempt 2's first gated reply does.
    if (Status settled = clock.sleep(kSettle, cancellation); !settled.has_value())
    {
        return settled;
    }
    auto trailing = transport.read(kSettleRead, cancellation);
    if (!trailing.has_value())
    {
        return std::unexpected(trailing.error());
    }
    if (trailing->has_value())
    {
        events.log(LogLevel::Debug, std::format("Discarded after kernel upload: {}", bytes::toHex(**trailing)));
    }
    return {};
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rBootModeKernelExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rBootModeKernel); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // execute() :52-60.
    KlineConfig config =
        non_iso14230_kline_config_from(std::get<SubaruUnisiaJecsM32rBootModeKernelPlan>(plan.family_plan()));
    config.parity = KlineParity::Even;
    return config;
}

Status SubaruUnisiaJecsM32rBootModeKernelExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                              const ICancellationToken&) const
{
    // execute() :52-53.
    if (Status reset = transport.reset_connection(); !reset.has_value())
    {
        return reset;
    }
    return transport.set_add_iso14230_header(false);
}

Result<FlashExecutionResult> SubaruUnisiaJecsM32rBootModeKernelExecutor::execute(
    const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rBootModeKernel); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const Status uploaded = upload(plan, transport, clock, cancellation, events);
    // Legacy dropped the lines implicitly, by closing the port in write_mem()'s
    // reset_connection() (:361); the explicit drop is the same on every adapter.
    events.log(LogLevel::Debug, "Removing boot mode voltages from Line End Check 1 and 2");
    const Status dropped = transport.disable_lec_lines();
    if (!uploaded.has_value())
    {
        return std::unexpected(uploaded.error());
    }
    if (!dropped.has_value())
    {
        return std::unexpected(dropped.error());
    }
    return FlashExecutionResult{.operation = FlashOperation::Write, .read_bytes = std::nullopt, .rom_id = std::nullopt};
}
} // namespace fastecu::flash
```

- [ ] **Step 5: Run the tests**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_kernel_executor_test && bazel build --config=release //:portable_closure`
Expected: PASS. If `CancellationAtEachCheckpointDropsTheLines` shows the check count off by one, print `context.cancellation` checks with a temporary log and correct the test's checkpoint numbers, not the executor's check order.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.h \
  src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.cpp \
  src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor_test.cpp \
  src/backend/flash/ecu/BUILD.bazel bazel/portable_targets.bzl
git commit -m "feat(flash): upload the Unisia Jecs M32R bootmode kernel through a portable executor

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 7: Erase-and-program executor (attempt 2)

**Files:**
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: Task 5 builders/validator; `SsmProtocol::addHeader`, `SsmProtocol::hasValidFrame` from `src/algorithms/protocol/ssm/ssm_protocol_core.h`.
- Produces: `class SubaruUnisiaJecsM32rBootModeProgramExecutor final : public IKlineFlashExecutor`.

- [ ] **Step 1: Write the failing tests**

`subaru_unisia_jecs_m32r_bootmode_program_executor_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using Line = ScriptedKlineFlashTransport::ControlLineAction;
using Op = ScriptedKlineFlashTransport::Operation;

// Built independently of SsmProtocol so the tests pin the wire bytes.
bytes::Bytes request(const bytes::Bytes& payload)
{
    bytes::Bytes frame{0x80, 0x10, 0xf0, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(bytes::sum8(frame));
    return frame;
}

bytes::Bytes reply(const bytes::Bytes& payload)
{
    bytes::Bytes frame{0x80, 0xf0, 0x10, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(bytes::sum8(frame));
    return frame;
}

bytes::Bytes rom(std::uint32_t size)
{
    bytes::Bytes image(size);
    for (std::uint32_t i = 0; i < size; ++i)
    {
        image[i] = static_cast<bytes::Byte>(i * 13 + 7);
    }
    return image;
}

FlashPlan program_plan(std::string_view protocol = "sub_ecu_unisia_jecs_20_bootmode",
                       std::string_view mcu = "M32R_128KB", std::uint32_t size = 0x20000)
{
    auto plan = build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation::Write, protocol, mcu, rom(size));
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

// write_mem() :485-512: AF 61|69 <addr24> <128 bytes as-is>.
bytes::Bytes block_request(const FlashPlan& plan, std::uint32_t block, bool last)
{
    const std::uint32_t address = block * 0x80;
    bytes::Bytes payload{0xaf, static_cast<bytes::Byte>(last ? 0x69 : 0x61), static_cast<bytes::Byte>(address >> 16U),
                         static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address)};
    payload.insert(payload.end(), plan.image()->begin() + address, plan.image()->begin() + address + 0x80);
    return request(payload);
}

// One empty poll in each loop, so both poll sleeps are exercised.
void script_erase(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("erase");
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queue_no_frame();
    transport.queueRead(reply({0xef, 0x42}));
    transport.queue_no_frame();
    transport.queueRead(reply({0xef, 0x52}));
}

void script_blocks(ScriptedKlineFlashTransport& transport, const FlashPlan& plan, std::uint32_t upto)
{
    auto section = transport.section("blocks");
    for (std::uint32_t block = 0; block < upto; ++block)
    {
        transport.exchange(block_request(plan, block, false), reply({0xef, 0x52}));
    }
}

struct RunContext
{
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
};

Result<FlashExecutionResult> run(const FlashPlan& plan, ScriptedKlineFlashTransport& transport, RunContext& context)
{
    return SubaruUnisiaJecsM32rBootModeProgramExecutor{}.execute(plan, transport, context.clock, context.cancellation,
                                                                 context.events);
}

bool logged(const RunContext& context, LogLevel level)
{
    return std::ranges::any_of(context.events.logs, [level](const auto& log) { return log.first == level; });
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, TransportSetupIs19200BaudNoParity)
{
    const auto setup = SubaruUnisiaJecsM32rBootModeProgramExecutor{}.transport_setup(program_plan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 19200);              // write_mem() :364
    EXPECT_EQ(setup->parity, KlineParity::None); // write_mem() :362
    EXPECT_FALSE(setup->iso14230);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, BeforeConfigureResetsThenClearsTheHeader)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(
        SubaruUnisiaJecsM32rBootModeProgramExecutor{}.before_transport_configure(transport, clock, cancellation),
        IsOk());
    EXPECT_EQ(transport.lifecycle_calls_, std::vector<std::string>{"reset_connection"}); // write_mem() :361
    EXPECT_EQ(transport.header_mode_calls_, std::vector<bool>{false});
}

class ProgramsTheWholeRom : public ::testing::TestWithParam<std::tuple<std::string_view, std::string_view, std::uint32_t>>
{
};

TEST_P(ProgramsTheWholeRom, ErasesThenWritesEveryBlock)
{
    const auto [protocol, mcu, size] = GetParam();
    const FlashPlan plan = program_plan(protocol, mcu, size);
    const std::uint32_t blocks = size / 0x80;
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    script_blocks(transport, plan, blocks - 1);
    transport.exchange(block_request(plan, blocks - 1, true), reply({0xef, 0x52}));
    RunContext context;

    ASSERT_THAT(run(plan, transport, context), IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_, (std::vector{Line::EnableProgrammingVoltageLine, Line::DisableLecLines}));
    EXPECT_EQ(transport.programming_voltage_line_write_index_, std::optional<std::size_t>(0));
    // AF 31 settle 500, one empty start poll 500, one empty done poll 1000,
    // post-erase 1000, then 10 ms after every AF 61 (:370-465, :539).
    EXPECT_EQ(context.clock.elapsed(), 500ms + 500ms + 1000ms + 1000ms + 10ms * (blocks - 1));
    EXPECT_EQ(std::ranges::count(transport.operation_trace_, Op::Read10), 4);
    EXPECT_EQ(context.events.progress_calls.back(), (std::pair<int, int>{static_cast<int>(blocks), static_cast<int>(blocks)}));
    EXPECT_FALSE(logged(context, LogLevel::Warning));
}

INSTANTIATE_TEST_SUITE_P(BothBootmodeRoms, ProgramsTheWholeRom,
                         ::testing::Values(std::make_tuple("sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000U),
                                           std::make_tuple("sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000U)));

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, SilenceAfterTheFinalBlockSucceedsWithAWarning)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    script_blocks(transport, plan, 1023);
    transport.expectWrite(block_request(plan, 1023, true));
    transport.queue_no_frame();
    RunContext context;
    ASSERT_THAT(run(plan, transport, context), IsOk());
    EXPECT_TRUE(logged(context, LogLevel::Warning));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, ABadFinalBlockReplyFails)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    script_blocks(transport, plan, 1023);
    transport.exchange(block_request(plan, 1023, true), reply({0xef, 0x5c}));
    RunContext context;
    const auto result = run(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("checksum error"));
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, EraseStartExhaustionFails)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.queue_no_frame();
    }
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(context.clock.elapsed(), 500ms + 20 * 500ms);
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, EraseDoneExhaustionFailsInsteadOfProgramming)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x42}));
    for (int round = 0; round < 20; ++round)
    {
        transport.queue_no_frame();
    }
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, AWrongEraseFrameFailsAndNamesTheStatus)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x48}));
    RunContext context;
    const auto result = run(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("missing VPP"));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, NamedAndUnknownStatusesInBlockReplies)
{
    for (const auto& [status, text] : std::to_array<std::pair<bytes::Byte, std::string_view>>({
             {0x72, "address error"},
             {0x8a, "FENTRY bit not set"},
             {0x5a, "unknown"},
         }))
    {
        const FlashPlan plan = program_plan();
        ScriptedKlineFlashTransport transport;
        script_erase(transport);
        transport.exchange(block_request(plan, 0, false), reply({0xef, status}));
        RunContext context;
        const auto result = run(plan, transport, context);
        ASSERT_THAT(result, IsErr(ErrorKind::BadResponse));
        EXPECT_THAT(result.error().detail, ::testing::HasSubstr(std::string(text)));
    }
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, SilenceAfterANonFinalBlockTimesOut)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    transport.expectWrite(block_request(plan, 0, false));
    transport.queue_no_frame();
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancelledBeforeVoltageNeverRaisesIt)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    RunContext context;
    context.cancellation.set_cancelled(true);
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.control_line_trace_, std::vector{Line::DisableLecLines});
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancelledMidProgrammingReportsCancelled)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    script_blocks(transport, plan, 3);
    RunContext context;
    context.cancellation.set_predicate([&transport] { return transport.writesConsumed() >= 3; });
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CleanupFailureNeverReplacesAnEarlierError)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x48}));
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::BadResponse));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, RejectsAKernelPlan)
{
    auto plan = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(
        FlashOperation::Write, "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", bytes::Bytes(0x80, 0x00));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeProgramExecutor{}.transport_setup(*plan),
                IsErr(ErrorKind::InvalidConfig));
}
} // namespace
} // namespace fastecu::flash
```

BUILD rules:

```python
cc_library(
    name = "subaru_unisia_jecs_m32r_bootmode_program_executor",
    srcs = ["subaru_unisia_jecs_m32r_bootmode_program_executor.cpp"],
    hdrs = ["subaru_unisia_jecs_m32r_bootmode_program_executor.h"],
    deps = [
        ":subaru_unisia_jecs_m32r_bootmode_plan",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/ssm",
        "//src/backend/flash:flash_executor",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_unisia_jecs_m32r_bootmode_program_executor_test",
    srcs = ["subaru_unisia_jecs_m32r_bootmode_program_executor_test.cpp"],
    deps = [
        ":subaru_unisia_jecs_m32r_bootmode_plan",
        ":subaru_unisia_jecs_m32r_bootmode_program_executor",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:recording_event_sink",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

Add `"subaru_unisia_jecs_m32r_bootmode_program_executor",` to `bazel/portable_targets.bzl`.

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_program_executor_test`
Expected: FAIL — missing header.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// Wave 7, attempt 2 of a bootmode Write: erases and programs through the
// kernel attempt 1 uploaded, with VPP raised and MOD1 dropped.
class SubaruUnisiaJecsM32rBootModeProgramExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Status before_transport_configure(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
```

- [ ] **Step 4: Write the implementation**

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;
using namespace std::chrono_literals;

// Legacy flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.{h,cpp}.
constexpr std::uint32_t kBlock = 0x80;        // write_mem() :469
constexpr auto kEraseSettle = 500ms;          // write_mem() :389
constexpr auto kPollRead = 10ms;              // write_mem() :400, :442
constexpr int kEraseRounds = 20;              // write_mem() :393, :435
constexpr auto kEraseStartSleep = 500ms;      // write_mem() :421
constexpr auto kEraseDoneSleep = 1000ms;      // write_mem() :462
constexpr auto kPostErase = 1000ms;           // write_mem() :465
constexpr auto kBlockTimeout = 3000ms;        // serial_read_extra_long_timeout, :518
constexpr auto kBlockPacing = 10ms;           // write_mem() :539

struct Session
{
    IKlineFlashTransport& transport;
    IClock& clock;
    const ICancellationToken& cancellation;
    IEventSink& events;
    const SubaruUnisiaJecsM32rBootModeProgramPlan& wire;
};

Status cancelled_if_requested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::format("cancelled {}", where));
    }
    return {};
}

Status send(Session& s, bytes::ByteView payload)
{
    if (Status cancelled = cancelled_if_requested(s.cancellation, "before write"); !cancelled.has_value())
    {
        return cancelled;
    }
    const bytes::Bytes request = SsmProtocol::addHeader(payload, s.wire.tester_id, s.wire.target_id);
    auto written = s.transport.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return fail(ErrorKind::Disconnected, "short K-Line write");
    }
    return {};
}

Result<std::optional<bytes::Bytes>> receive(Session& s, std::chrono::milliseconds timeout)
{
    auto response = s.transport.read(timeout, s.cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (Status cancelled = cancelled_if_requested(s.cancellation, "after read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    return std::move(*response);
}

bool is_status_reply(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire)
{
    return SsmProtocol::hasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] == 2 && frame[4] == 0xef;
}

bool is_status(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire, bytes::Byte status)
{
    return is_status_reply(frame, wire) && frame[5] == status;
}

// write_mem() :346-353 documents these as error codes; success replies are
// EF 42 and EF 52, so they are read as the status byte after EF.
std::string_view status_meaning(bytes::Byte status)
{
    switch (status)
    {
    case 0x42:
        return "erase started";
    case 0x48:
        return "missing VPP voltage";
    case 0x52:
        return "done";
    case 0x5c:
        return "checksum error";
    case 0x72:
        return "address error";
    case 0x8a:
        return "FENTRY bit not set";
    default:
        return "unknown";
    }
}

std::string describe(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire)
{
    if (is_status_reply(frame, wire))
    {
        return std::format("{} (status {:02X}: {})", bytes::toHex(frame), frame[5], status_meaning(frame[5]));
    }
    return bytes::toHex(frame);
}

// write_mem() :393-463. read() returns whole frames: an empty read continues
// the poll, and any frame must be EF <status>. Legacy's first poll fell
// through on one to six bytes and its second never failed.
Status poll_for(Session& s, bytes::Byte status, std::chrono::milliseconds sleep, std::string_view what)
{
    for (int round = 0; round < kEraseRounds; ++round)
    {
        auto response = receive(s, kPollRead);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->has_value())
        {
            if (!is_status(**response, s.wire, status))
            {
                return fail(ErrorKind::BadResponse, std::format("{} failed: {}", what, describe(**response, s.wire)));
            }
            return {};
        }
        if (Status slept = s.clock.sleep(sleep, s.cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return fail(ErrorKind::Timeout, std::format("no {} response after {} polls", what, kEraseRounds));
}

Status program(Session& s, const FlashPlan& plan)
{
    if (Status cancelled = cancelled_if_requested(s.cancellation, "before programming voltage"); !cancelled.has_value())
    {
        return cancelled;
    }
    // write_mem() :366: VPP stays, MOD1 drops. The operator removed MOD1
    // before this attempt started (the workflow's RemoveMod1 prompt).
    s.events.log(LogLevel::Debug, "Set programming voltage +12v to Line End Check 1");
    if (Status raised = s.transport.enable_programming_voltage_line(); !raised.has_value())
    {
        return raised;
    }

    s.events.log(LogLevel::Info, "Requesting flash erase, please wait...");
    if (Status sent = send(s, composeBe(0xaf_b, 0x31_b)); !sent.has_value())
    {
        return sent;
    }
    if (Status settled = s.clock.sleep(kEraseSettle, s.cancellation); !settled.has_value())
    {
        return settled;
    }
    if (Status started = poll_for(s, 0x42, kEraseStartSleep, "flash erase start"); !started.has_value())
    {
        return started;
    }
    s.events.log(LogLevel::Info, "Flash erase in progress, please wait...");
    if (Status erased = poll_for(s, 0x52, kEraseDoneSleep, "flash erase"); !erased.has_value())
    {
        return erased;
    }
    s.events.log(LogLevel::Info, "Flash erased!");
    if (Status settled = s.clock.sleep(kPostErase, s.cancellation); !settled.has_value())
    {
        return settled;
    }

    // write_mem() :467-578. Data goes as-is; unlike 6c-3 there is no XOR.
    const bytes::Bytes& image = *plan.image();
    const auto blocks = static_cast<int>(image.size() / kBlock);
    for (int block = 0; block < blocks; ++block)
    {
        const std::uint32_t address = static_cast<std::uint32_t>(block) * kBlock;
        const bool last = block == blocks - 1;
        const bytes::ByteView data = bytes::ByteView(image).subspan(address, kBlock);
        if (Status sent = send(s, composeBe(0xaf_b, last ? 0x69_b : 0x61_b, u24(address), data)); !sent.has_value())
        {
            return sent;
        }
        auto response = receive(s, kBlockTimeout);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (!response->has_value())
        {
            if (!last)
            {
                return fail(ErrorKind::Timeout, std::format("no response to block write at 0x{:06X}", address));
            }
            // Legacy never read a reply to AF 69 (:517); its shape is unknown.
            s.events.log(LogLevel::Warning, "No reply to the final block; treating the write as complete");
        }
        else if (!is_status(**response, s.wire, 0x52))
        {
            return fail(ErrorKind::BadResponse, std::format("block write at 0x{:06X} failed: {}", address,
                                                            describe(**response, s.wire)));
        }
        s.events.progress(block + 1, blocks);
        if (!last)
        {
            if (Status paced = s.clock.sleep(kBlockPacing, s.cancellation); !paced.has_value())
            {
                return paced;
            }
        }
    }
    s.events.log(LogLevel::Info, "ROM written to flash.");
    // write_mem() :581.
    s.events.log(LogLevel::Info, "Please remove VPP voltage, power cycle ECU and request SSM Init to confirm.");
    return {};
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rBootModeProgramExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rBootModeProgram); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // write_mem() :361-364: no parity, 19200 baud.
    return non_iso14230_kline_config_from(std::get<SubaruUnisiaJecsM32rBootModeProgramPlan>(plan.family_plan()));
}

Status SubaruUnisiaJecsM32rBootModeProgramExecutor::before_transport_configure(IKlineFlashTransport& transport,
                                                                               IClock&,
                                                                               const ICancellationToken&) const
{
    // write_mem() :361.
    if (Status reset = transport.reset_connection(); !reset.has_value())
    {
        return reset;
    }
    return transport.set_add_iso14230_header(false);
}

Result<FlashExecutionResult> SubaruUnisiaJecsM32rBootModeProgramExecutor::execute(
    const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rBootModeProgram); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    Session session{transport, clock, cancellation, events,
                    std::get<SubaruUnisiaJecsM32rBootModeProgramPlan>(plan.family_plan())};
    const Status written = program(session, plan);
    // execute() :85, :89: legacy dropped the lines after write_mem().
    events.log(LogLevel::Debug, "Removing programming voltage +12v from Line End Check 1");
    const Status dropped = transport.disable_lec_lines();
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (!dropped.has_value())
    {
        return std::unexpected(dropped.error());
    }
    return FlashExecutionResult{.operation = FlashOperation::Write, .read_bytes = std::nullopt, .rom_id = std::nullopt};
}
} // namespace fastecu::flash
```

- [ ] **Step 5: Run the tests**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_program_executor_test && bazel build --config=release //:portable_closure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h \
  src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.cpp \
  src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor_test.cpp \
  src/backend/flash/ecu/BUILD.bazel bazel/portable_targets.bzl
git commit -m "feat(flash): erase and program Unisia Jecs M32R bootmode ROMs with gated polls

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 8: Workflow — prompt kinds, kernel bytes, routes, two-attempt Write

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.h:35-53`
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp` (includes, `resolveKernelBytes` after `resolveKernel` at :188-208, new workflow class after `SubaruUnisiaJecsM32rKlineWorkflow`, `Route::Kind`, `kRoutes`, factory switch)
- Modify: `src/platform/desktop/common/flash/BUILD.bazel` (workflow deps)
- Test: `src/platform/desktop/common/flash/flash_workflow_test.cpp`

**Interfaces:**
- Consumes: Tasks 3, 5, 6, 7.
- Produces:
  - `FlashPromptKind::ApplyBootModeVoltages`, `FlashPromptKind::RemoveMod1`.
  - `RemoveProgrammingVoltage` argument `power_off_advice` (`"no"` from bootmode; absent elsewhere).
  - Routes `sub_ecu_unisia_jecs_{20,30}_bootmode` (Exact) → `SubaruUnisiaJecsM32rBootModeWorkflow`.

- [ ] **Step 1: Write the failing workflow tests**

Add to the catalog in `catalogPaths()` before `</protocols>`:

```xml
    <protocol name="sub_ecu_unisia_jecs_20_bootmode">
      <ecu>WA12212920WWW</ecu><mcu>M32R_128KB</mcu>
      <kernel>catalog_uj20_bootmode.bin</kernel>
    </protocol>
    <protocol name="sub_ecu_unisia_jecs_30_bootmode">
      <ecu>WA12212930WWW</ecu><mcu>M32R_256KB</mcu>
      <kernel>catalog_uj30_bootmode.bin</kernel>
    </protocol>
```

and to the kernel-file writes:

```cpp
            !writeFile(kernel_directory + "/catalog_uj20_bootmode.bin", QByteArray::fromHex("0102030405")) ||
            !writeFile(kernel_directory + "/catalog_uj30_bootmode.bin", QByteArray::fromHex("0607")) ||
```

After `unisiaM32rWriteAtAttempt()`, add helpers:

```cpp
FlashWorkflowRequest unisiaBootmodeWrite(const config::ConfigPaths& paths)
{
    auto input = request("sub_ecu_unisia_jecs_20_bootmode", FlashOperation::Write);
    input.mcu = "M32R_128KB";
    input.image = bytes::Bytes(0x20000, 0xa5);
    input.paths = paths;
    return input;
}

// Begin -> ApplyBootModeVoltages -> kernel attempt, all accepted.
std::unique_ptr<FlashWorkflow> unisiaBootmodeAtKernelAttempt(const config::ConfigPaths& paths)
{
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(paths));
    if (workflow == nullptr || std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::Begin)
    {
        return nullptr;
    }
    workflow->submit(FlashPromptResponse::Accept);
    if (std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::ApplyBootModeVoltages)
    {
        return nullptr;
    }
    workflow->submit(FlashPromptResponse::Accept);
    if (!std::holds_alternative<FlashAttempt>(workflow->next()))
    {
        return nullptr;
    }
    return workflow;
}

const PromptArguments kBootmodeNotice(std::string outcome)
{
    return {{"outcome", std::move(outcome)}, {"external_vpp", "yes"}, {"power_off_advice", "no"}};
}
```

(`PromptArguments` is declared just below `unisiaM32rWriteAtAttempt`; place these helpers after its declaration.)

Replace the slot declaration `void unisiaJecsM32rBootmodeAndLookalikesStayLegacy();` with:

```cpp
    void unisiaJecsM32rLookalikesStayUnrouted();
    void unisiaBootmodeReadUsesTheKlineReadFamily();
    void unisiaBootmodeWriteRunsKernelThenMod1ThenProgram();
    void unisiaBootmodeKernelFailureSkipsMod1AndProgram();
    void unisiaBootmodeKernelCancelledShowsNotice();
    void unisiaBootmodeDeclinedMod1CancelsWithNotice();
    void unisiaBootmodeProgramFailureShowsNoticeThenFailure();
    void unisiaBootmodeDeclinedVoltagesCancelsBeforeAnyAttempt();
    void unisiaBootmodeWrongImageSizeFailsBeforeAnyPrompt();
    void unisiaBootmodeMissingKernelFailsBeforeAnyPrompt();
    void unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt();
    void unisiaBootmodeTestWriteIsUnsupported();
```

Replace the body of `unisiaJecsM32rBootmodeAndLookalikesStayLegacy` with these definitions:

```cpp
void FlashWorkflowTest::unisiaJecsM32rLookalikesStayUnrouted()
{
    for (const char *protocol : {"sub_ecu_unisia_jecs_20x", "sub_ecu_unisia_jecs_7",
                                 "sub_ecu_unisia_jecs_20_bootmodex", "sub_ecu_unisia_jecs_40_bootmode"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) == nullptr, protocol);
    }
}

void FlashWorkflowTest::unisiaBootmodeReadUsesTheKlineReadFamily()
{
    struct Variant
    {
        const char *protocol;
        const char *mcu;
        std::uint32_t rom_size;
    };
    for (const Variant& variant : std::to_array<Variant>({
             {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
             {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
         }))
    {
        auto input = request(variant.protocol);
        input.mcu = variant.mcu;
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY2(workflow != nullptr, variant.protocol);
        QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        auto step = workflow->next();
        QVERIFY2(std::holds_alternative<FlashAttempt>(step), variant.protocol);
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        QCOMPARE(plan.family(), FlashFamily::SubaruUnisiaJecsM32rKline);
        QCOMPARE(plan.transfer_region(), (MemoryRegion{0x100000, variant.rom_size}));
        QVERIFY(plan.confirmations().empty());
        workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1},
                                            .rom_id = std::string("123456789A_")});
        const auto done = workflow->next();
        QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
        QVERIFY(std::get<FlashCompletedStep>(done).rom_id == std::optional<std::string>("123456789A_"));
    }
}

void FlashWorkflowTest::unisiaBootmodeWriteRunsKernelThenMod1ThenProgram()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(*paths));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        QFAIL(failure->error.detail.c_str());
    }
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ApplyBootModeVoltages);
    workflow->submit(FlashPromptResponse::Accept);

    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& kernel = std::get<FlashAttempt>(step).attempt->plan();
    QCOMPARE(kernel.family(), FlashFamily::SubaruUnisiaJecsM32rBootModeKernel);
    bytes::Bytes padded{0x01, 0x02, 0x03, 0x04, 0x05};
    padded.resize(0x80, 0x00);
    QCOMPARE(kernel.image(), std::optional<bytes::Bytes>(padded));
    workflow->submit(FlashAttemptResult{.success = true});

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::RemoveMod1);
    workflow->submit(FlashPromptResponse::Accept);

    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& program = std::get<FlashAttempt>(step).attempt->plan();
    QCOMPARE(program.family(), FlashFamily::SubaruUnisiaJecsM32rBootModeProgram);
    QCOMPARE(program.image(), std::optional<bytes::Bytes>(bytes::Bytes(0x20000, 0xa5)));
    workflow->submit(FlashAttemptResult{.success = true});

    const auto notice = std::get<FlashPromptStep>(workflow->next());
    QCOMPARE(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    QVERIFY(notice.arguments == kBootmodeNotice("succeeded"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
}

void FlashWorkflowTest::unisiaBootmodeKernelFailureSkipsMod1AndProgram()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto workflow = unisiaBootmodeAtKernelAttempt(*paths);
    QVERIFY(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::InvalidConfig, .error_detail = "baud"});
    const auto notice = std::get<FlashPromptStep>(workflow->next());
    QCOMPARE(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    QVERIFY(notice.arguments == kBootmodeNotice("failed"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto failure = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(failure));
    QCOMPARE(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::InvalidConfig);
}

void FlashWorkflowTest::unisiaBootmodeKernelCancelledShowsNotice()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto workflow = unisiaBootmodeAtKernelAttempt(*paths);
    QVERIFY(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::Cancelled});
    const auto notice = std::get<FlashPromptStep>(workflow->next());
    QCOMPARE(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    QVERIFY(notice.arguments == kBootmodeNotice("cancelled"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::unisiaBootmodeDeclinedMod1CancelsWithNotice()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto workflow = unisiaBootmodeAtKernelAttempt(*paths);
    QVERIFY(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = true});
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::RemoveMod1);
    workflow->submit(FlashPromptResponse::Decline);
    const auto notice = std::get<FlashPromptStep>(workflow->next());
    QCOMPARE(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    QVERIFY(notice.arguments == kBootmodeNotice("cancelled"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::unisiaBootmodeProgramFailureShowsNoticeThenFailure()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto workflow = unisiaBootmodeAtKernelAttempt(*paths);
    QVERIFY(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = true});
    workflow->next(); // RemoveMod1
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::BadResponse, .error_detail = "x"});
    const auto notice = std::get<FlashPromptStep>(workflow->next());
    QVERIFY(notice.arguments == kBootmodeNotice("failed"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto failure = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(failure));
    QCOMPARE(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::BadResponse);
}

void FlashWorkflowTest::unisiaBootmodeDeclinedVoltagesCancelsBeforeAnyAttempt()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(*paths));
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ApplyBootModeVoltages);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::unisiaBootmodeWrongImageSizeFailsBeforeAnyPrompt()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto input = unisiaBootmodeWrite(*paths);
    input.image = bytes::Bytes(0x20001, 0xa5);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

void FlashWorkflowTest::unisiaBootmodeMissingKernelFailsBeforeAnyPrompt()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory, false);
    QVERIFY(paths.has_value());
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(*paths));
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QVERIFY(std::get<FlashFailureStep>(step).error.detail.find("catalog_uj20_bootmode.bin") != std::string::npos);
}

void FlashWorkflowTest::unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    QVERIFY(writeFile(directory.filePath("kernels/catalog_uj20_bootmode.bin"), QByteArray()));
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(*paths));
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

void FlashWorkflowTest::unisiaBootmodeTestWriteIsUnsupported()
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto input = unisiaBootmodeWrite(*paths);
    input.operation = FlashOperation::TestWrite;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}
```

If `unisiaBootmodeMissingKernelFailsBeforeAnyPrompt` fails because the repository's error detail does not include the file name, make `resolveKernelBytes` wrap it: `fail(error.kind, std::format("kernel file '{}': {}", entry->kernel, error.detail))`.

- [ ] **Step 2: Run to see them fail**

Run: `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow`
Expected: compile error on `FlashPromptKind::ApplyBootModeVoltages`.

- [ ] **Step 3: Add the prompt kinds**

In `flash_workflow.h`, update the `RemoveProgrammingVoltage` comment and append kinds:

```cpp
    // Wave 6c-3. After a write attempt, OK-only. Due after every write that
    // did not succeed, whatever the adapter, and after a successful write
    // when external VPP was needed. Argument "outcome" is "succeeded",
    // "failed" or "cancelled"; "external_vpp" is "yes" when the operator
    // applied VPP (the notice asks for its removal) or "no" otherwise. Every
    // outcome other than "succeeded" carries the don't-power-off advice
    // unless "power_off_advice" is "no" (wave 7 bootmode: the boot ROM is
    // always re-enterable, so a power cycle is the recovery path).
    RemoveProgrammingVoltage,
    // Wave 7. Before a bootmode write: the operator connects VPP and MOD1.
    ApplyBootModeVoltages,
    // Wave 7. Between the bootmode kernel upload and programming, OK/Cancel.
    RemoveMod1,
```

- [ ] **Step 4: Add `resolveKernelBytes`**

After `resolveKernel()` in `flash_workflow.cpp`:

```cpp
// The cfg <kernel> file alone. The Unisia Jecs M32R _bootmode entries declare
// no <kernel_addr>: the M32R boot ROM places the kernel itself.
Result<bytes::Bytes> resolveKernelBytes(const FlashWorkflowRequest& request, IFileRepository& repository)
{
    Result<config::ProtocolEntry> entry = resolveProtocol(request.paths, request.protocol, repository);
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    Result<std::vector<std::uint8_t>> kernel_bytes =
        repository.read(request.paths.kernel_files_directory + entry->kernel);
    if (!kernel_bytes.has_value())
    {
        return std::unexpected(kernel_bytes.error());
    }
    return bytes::Bytes(kernel_bytes->begin(), kernel_bytes->end());
}
```

- [ ] **Step 5: Add the workflow class**

Add includes beside the other Unisia Jecs ones:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h"
```

After `SubaruUnisiaJecsM32rKlineWorkflow`:

```cpp
// Wave 7. Read is the 6c-3 K-Line read. Write is two attempts with an
// operator step between them, where legacy reset the connection anyway
// (flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp:361-367):
// kernel upload, RemoveMod1, erase and program. Both plans are built before
// Begin so a missing kernel or a wrong-size image fails before any prompt.
class SubaruUnisiaJecsM32rBootModeWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruUnisiaJecsM32rBootModeWorkflow(FlashWorkflowRequest request) : request_(std::move(request))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!built_.has_value())
        {
            built_ = buildPlans();
        }
        if (!built_->has_value())
        {
            return FlashFailureStep{built_->error()};
        }
        // The notice precedes whatever the attempt produced, failure included.
        if (stage_ == Stage::Notice)
        {
            return FlashPromptStep{
                FlashPromptKind::RemoveProgrammingVoltage,
                {{"outcome", notice_outcome_}, {"external_vpp", "yes"}, {"power_off_advice", "no"}}};
        }
        if (outcome_.hasFailure())
        {
            return outcome_.takeFailure();
        }
        if (outcome_.terminal())
        {
            return outcome_.completedStep();
        }
        switch (stage_)
        {
        case Stage::Begin:
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        case Stage::ApplyVoltages:
            return FlashPromptStep{FlashPromptKind::ApplyBootModeVoltages, {}};
        case Stage::FirstAttempt:
            stage_ = Stage::AwaitFirst;
            return firstAttempt();
        case Stage::RemoveMod1:
            return FlashPromptStep{FlashPromptKind::RemoveMod1, {}};
        case Stage::ProgramAttempt:
            stage_ = Stage::AwaitProgram;
            return attempt(std::move(*(*built_)->program), std::make_unique<SubaruUnisiaJecsM32rBootModeProgramExecutor>());
        case Stage::AwaitFirst:
        case Stage::AwaitProgram:
        case Stage::Notice:
        case Stage::Done:
            break;
        }
        return outcome_.completedStep();
    }

    void submit(FlashPromptResponse response) override
    {
        switch (stage_)
        {
        case Stage::Begin:
        case Stage::ApplyVoltages:
            if (response != FlashPromptResponse::Accept)
            {
                outcome_.cancel();
                return;
            }
            stage_ = stage_ == Stage::Begin && is_write() ? Stage::ApplyVoltages : Stage::FirstAttempt;
            return;
        case Stage::RemoveMod1:
            if (response != FlashPromptResponse::Accept)
            {
                // The kernel runs and nothing is erased; still ask for VPP removal.
                notice_outcome_ = "cancelled";
                stage_ = Stage::Notice;
                outcome_.cancel();
                return;
            }
            stage_ = Stage::ProgramAttempt;
            return;
        case Stage::Notice:
            stage_ = Stage::Done; // OK-only notice
            return;
        case Stage::FirstAttempt:
        case Stage::AwaitFirst:
        case Stage::ProgramAttempt:
        case Stage::AwaitProgram:
        case Stage::Done:
            return;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        if (!is_write())
        {
            outcome_.record(std::move(result));
            return;
        }
        // A successful kernel upload is not an outcome yet: RemoveMod1 follows.
        if (stage_ == Stage::AwaitFirst && result.success)
        {
            stage_ = Stage::RemoveMod1;
            return;
        }
        notice_outcome_ = result.success                              ? "succeeded"
                          : result.error_kind == ErrorKind::Cancelled ? "cancelled"
                                                                      : "failed";
        stage_ = Stage::Notice;
        outcome_.record(std::move(result));
    }

  private:
    enum class Stage
    {
        Begin,
        ApplyVoltages,
        FirstAttempt,
        AwaitFirst,
        RemoveMod1,
        ProgramAttempt,
        AwaitProgram,
        Notice,
        Done,
    };

    struct Plans
    {
        std::optional<FlashPlan> first;   // Read plan, or the kernel upload
        std::optional<FlashPlan> program; // Write only
    };

    bool is_write() const
    {
        return request_.operation != FlashOperation::Read;
    }

    Result<Plans> buildPlans()
    {
        if (!is_write())
        {
            // adapter_supplies_programming_voltage is irrelevant to Read.
            auto read = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, request_.protocol, request_.mcu,
                                                                 std::nullopt, true);
            if (!read.has_value())
            {
                return std::unexpected(read.error());
            }
            return Plans{std::move(*read), std::nullopt};
        }
        // Program first: it needs no I/O and rejects TestWrite and a wrong
        // image before the kernel file is read.
        auto program = build_subaru_unisia_jecs_m32r_bootmode_program_plan(request_.operation, request_.protocol,
                                                                           request_.mcu, std::move(request_.image));
        if (!program.has_value())
        {
            return std::unexpected(program.error());
        }
        QtFileRepository repository;
        Result<bytes::Bytes> kernel_bytes = resolveKernelBytes(request_, repository);
        if (!kernel_bytes.has_value())
        {
            return std::unexpected(kernel_bytes.error());
        }
        auto kernel = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(request_.operation, request_.protocol,
                                                                         request_.mcu, std::move(*kernel_bytes));
        if (!kernel.has_value())
        {
            return std::unexpected(kernel.error());
        }
        return Plans{std::move(*kernel), std::move(*program)};
    }

    FlashWorkflowStep firstAttempt()
    {
        FlashPlan plan = std::move(*(*built_)->first);
        if (!is_write())
        {
            return attempt(std::move(plan), std::make_unique<SubaruUnisiaJecsM32rKlineExecutor>());
        }
        return attempt(std::move(plan), std::make_unique<SubaruUnisiaJecsM32rBootModeKernelExecutor>());
    }

    template <typename Executor> FlashWorkflowStep attempt(FlashPlan plan, std::unique_ptr<Executor> executor)
    {
        return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                 bind_flash_attempt(std::move(plan), std::move(executor),
                                                    std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                 std::make_unique<QtClock>()};
    }

    FlashWorkflowRequest request_;
    std::optional<Result<Plans>> built_;
    Stage stage_ = Stage::Begin;
    std::string notice_outcome_;
    FlashAttemptOutcome outcome_;
};
```

- [ ] **Step 6: Route the two names**

Add `SubaruUnisiaJecsM32rBootMode,` to `Route::Kind` before `Unrouted`. In `kRoutes`, replace the "Exact only" comment and add rows:

```cpp
    // Exact only: the _bootmode names share these prefixes.
    {"sub_ecu_unisia_jecs_20", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_30", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_40", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_70", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_20_bootmode", SubaruUnisiaJecsM32rBootMode, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_30_bootmode", SubaruUnisiaJecsM32rBootMode, RouteMatch::Exact},
```

In the factory switch, before `case Unrouted:`:

```cpp
    case SubaruUnisiaJecsM32rBootMode:
        return std::make_unique<SubaruUnisiaJecsM32rBootModeWorkflow>(std::move(request));
```

In `src/platform/desktop/common/flash/BUILD.bazel`, add to the `flash_workflow` library deps beside the other Unisia Jecs M32R entries:

```python
        "//src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_kernel_executor",
        "//src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_plan",
        "//src/backend/flash/ecu:subaru_unisia_jecs_m32r_bootmode_program_executor",
```

- [ ] **Step 7: Run the tests**

Run: `bazel test --config=release //src/platform/desktop/common/flash:all`
Expected: PASS, including every existing `unisiaJecsM32r*` 6c-3 test unchanged.

- [ ] **Step 8: Commit**

```bash
git add src/platform/desktop/common/flash/flash_workflow.h src/platform/desktop/common/flash/flash_workflow.cpp \
  src/platform/desktop/common/flash/flash_workflow_test.cpp src/platform/desktop/common/flash/BUILD.bazel
git commit -m "feat(flash): route Unisia Jecs M32R bootmode through a two-attempt workflow

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 9: Dialog rendering for the bootmode prompts

**Files:**
- Modify: `src/ui/desktop/flash/common/flash_dialog.h`, `flash_dialog.cpp:217-239`
- Test: `src/ui/desktop/flash/common/flash_dialog_test.cpp`

**Interfaces:**
- Consumes: Task 8 prompt kinds and `power_off_advice`.
- Produces:
  ```cpp
  struct ProgrammingVoltageNotice { QString title; QString text; };
  static ProgrammingVoltageNotice FlashDialog::programmingVoltageNotice(const FlashPromptStep& prompt);
  ```

- [ ] **Step 1: Write the failing dialog tests**

Add to `flash_dialog_test.cpp`, after `BlockingAttempt`:

```cpp
// Completes at once; lets a dialog test run a workflow with several attempts.
class InstantAttempt final : public BoundFlashAttempt
{
  public:
    explicit InstantAttempt(FlashPlan plan) : plan_(std::move(plan))
    {
    }
    const FlashPlan& plan() const noexcept override
    {
        return plan_;
    }
    Result<FlashExecutionResult> run(IClock&, const ICancellationToken&, IEventSink&) override
    {
        return FlashExecutionResult{.operation = FlashOperation::Write, .read_bytes = std::nullopt, .rom_id = std::nullopt};
    }
    void request_unblock() noexcept override
    {
    }

  private:
    FlashPlan plan_;
};

// Begin -> attempt -> RemoveMod1 -> attempt -> completed: the bootmode shape.
class TwoAttemptWorkflow final : public FlashWorkflow
{
  public:
    FlashWorkflowStep next() override
    {
        if (step_ == 0)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (step_ == 2)
        {
            return FlashPromptStep{FlashPromptKind::RemoveMod1, {}};
        }
        if (step_ == 1 || step_ == 3)
        {
            ++step_;
            auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_20",
                                                                 "M32R_128KB", std::nullopt, true);
            if (!plan.has_value())
            {
                return FlashFailureStep{plan.error()};
            }
            return FlashAttempt{std::make_unique<InstantAttempt>(std::move(*plan)), std::make_unique<FakeClock>()};
        }
        return FlashCompletedStep{FlashWorkflowOutcome::Succeeded, std::nullopt, std::nullopt};
    }
    void submit(FlashPromptResponse) override
    {
        ++step_;
    }
    void submit(FlashAttemptResult result) override
    {
        attempts.push_back(result.success);
    }

    QList<bool> attempts;

  private:
    int step_ = 0;
};
```

Add slots to `FlashDialogTest`:

```cpp
    void runsASecondAttemptAfterAPromptBetweenAttempts()
    {
        auto owned = std::make_unique<TwoAttemptWorkflow>();
        TwoAttemptWorkflow *workflow = owned.get();
        RecordingDialog dialog(std::move(owned), FlashOperation::Write, "rom.bin");
        const FlashDialogResult result = dialog.run();
        QCOMPARE(result.outcome, FlashWorkflowOutcome::Succeeded);
        QCOMPARE(dialog.prompts, (QList{FlashPromptKind::Begin, FlashPromptKind::RemoveMod1}));
        QCOMPARE(workflow->attempts, (QList{true, true}));
        QVERIFY(dialog.success_shown);
    }

    void programmingVoltageNoticeKeepsTheSixC3AdviceByDefault()
    {
        const auto notice = FlashDialog::programmingVoltageNotice(
            {FlashPromptKind::RemoveProgrammingVoltage, {{"outcome", "failed"}, {"external_vpp", "yes"}}});
        QCOMPARE(notice.title, QString("Programming voltage"));
        QVERIFY(notice.text.contains("Remove VPP voltage"));
        QVERIFY(notice.text.contains("do not power it off"));
    }

    void programmingVoltageNoticeWithoutPowerOffAdviceOnFailure()
    {
        const auto notice = FlashDialog::programmingVoltageNotice(
            {FlashPromptKind::RemoveProgrammingVoltage,
             {{"outcome", "failed"}, {"external_vpp", "yes"}, {"power_off_advice", "no"}}});
        QVERIFY(notice.text.contains("Remove VPP voltage"));
        QVERIFY(!notice.text.contains("do not power it off"));
        QVERIFY(notice.text.contains("try again"));
    }

    void programmingVoltageNoticeWithoutPowerOffAdviceOnSuccess()
    {
        const auto notice = FlashDialog::programmingVoltageNotice(
            {FlashPromptKind::RemoveProgrammingVoltage,
             {{"outcome", "succeeded"}, {"external_vpp", "yes"}, {"power_off_advice", "no"}}});
        QVERIFY(notice.text.contains("Remove VPP voltage"));
        QVERIFY(notice.text.contains("request SSM Init"));
        QVERIFY(!notice.text.contains("did not complete"));
    }
```

- [ ] **Step 2: Run to see them fail**

Run: `bazel test --config=release //src/ui/desktop/flash/common:all`
Expected: compile error on `programmingVoltageNotice`.

- [ ] **Step 3: Declare the notice helper**

In `flash_dialog.h`, after `FlashDialogResult`:

```cpp
struct ProgrammingVoltageNotice
{
    QString title;
    QString text;
};
```

and in `FlashDialog`'s public section after `run()`:

```cpp
    // The RemoveProgrammingVoltage notice's title and text; see the prompt
    // kind's arguments in flash_workflow.h.
    static ProgrammingVoltageNotice programmingVoltageNotice(const FlashPromptStep& prompt);
```

- [ ] **Step 4: Implement and render**

In `flash_dialog.cpp`, add before `presentPrompt`:

```cpp
ProgrammingVoltageNotice FlashDialog::programmingVoltageNotice(const FlashPromptStep& prompt)
{
    auto arg = [&prompt](std::string_view key)
    {
        for (const auto& [name, value] : prompt.arguments)
        {
            if (name == key)
            {
                return value;
            }
        }
        return std::string{};
    };
    const bool external_vpp = arg("external_vpp") == "yes";
    const bool succeeded = arg("outcome") == "succeeded";
    const bool power_off_advice = arg("power_off_advice") != "no";
    QStringList paragraphs;
    if (external_vpp)
    {
        paragraphs << tr("Remove VPP voltage from the ECU, then press OK.");
    }
    if (!succeeded && power_off_advice)
    {
        paragraphs << tr("The write did not complete. If the ECU entered flash mode, do not power it off: the "
                         "flash kernel is still running and you can try flashing again.");
    }
    else if (!succeeded)
    {
        // Legacy bootmode dialog: "ECU operation failed, press OK to exit and try again".
        paragraphs << tr("The write did not complete. Press OK to exit and try again.");
    }
    else if (!power_off_advice)
    {
        // Legacy bootmode write_mem() :581.
        paragraphs << tr("Power cycle the ECU and request SSM Init to confirm the write.");
    }
    return {external_vpp ? tr("Programming voltage") : tr("ECU operation"), paragraphs.join(QStringLiteral("\n\n"))};
}
```

Replace the `RemoveProgrammingVoltage` branch in `presentPrompt` with:

```cpp
    if (prompt.kind == FlashPromptKind::RemoveProgrammingVoltage)
    {
        const ProgrammingVoltageNotice notice = programmingVoltageNotice(prompt);
        QMessageBox::information(this, notice.title, notice.text);
        return FlashPromptResponse::Accept;
    }
    if (prompt.kind == FlashPromptKind::ApplyBootModeVoltages)
    {
        // Legacy flash_ecu_subaru_unisia_jecs_m32r_bootmode.cpp:38-41.
        return QMessageBox::warning(this, tr("Connecting to ECU"),
                                    tr("Connect VPP and MOD1 to the ECU, turn ignition ON, then press OK to continue."),
                                    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Ok
                   ? FlashPromptResponse::Accept
                   : FlashPromptResponse::Decline;
    }
    if (prompt.kind == FlashPromptKind::RemoveMod1)
    {
        // Legacy write_mem() :367 offered OK only; Cancel stops before erase.
        return QMessageBox::warning(this, tr("Flash file"),
                                    tr("The kernel is running. Remove MOD1 voltage, then press OK to erase and "
                                       "program the ECU, or Cancel to stop before anything is erased."),
                                    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Ok
                   ? FlashPromptResponse::Accept
                   : FlashPromptResponse::Decline;
    }
```

Add `#include <string>` and `#include <string_view>` to `flash_dialog.cpp`.

- [ ] **Step 5: Run the tests**

Run: `bazel test --config=release //src/ui/desktop/flash/common:all`
Expected: PASS, including `closingMidAttemptSubmitsCancelledAndPresentsTheNotice`.

- [ ] **Step 6: Commit**

```bash
git add src/ui/desktop/flash/common/flash_dialog.h src/ui/desktop/flash/common/flash_dialog.cpp \
  src/ui/desktop/flash/common/flash_dialog_test.cpp
git commit -m "feat(flash): render the Unisia Jecs M32R bootmode prompts in the flash dialog

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 10: Delete the legacy bootmode path

**Files:**
- Modify: `src/ui/desktop/mainwindow.cpp:1243-1255`, `src/ui/desktop/mainwindow.h:59-60`, `src/ui/desktop/BUILD.bazel:107`
- Delete: `src/ui/desktop/flash/bootmode/` (3 files)
- Delete: `src/platform/desktop/common/flash/legacy/bootmode/` (2 files)
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`
- Modify: `scripts/check-legacy-flash-drain.py:32-35`, `scripts/check-serial-compat-allowlist.py:31`, `src/platform/desktop/common/serial/BUILD.bazel:81`

**Interfaces:** none produced; the guards are the tests.

- [ ] **Step 1: Shrink the ratchets first and watch them fail**

In `check-legacy-flash-drain.py`:

```python
# Regenerate ONLY by removing entries, one per migrated family. Empty since
# wave 7; 7b deletes the package and this script together.
REMAINING: set[str] = set()
```

Remove `"//src/ui/desktop/flash/bootmode:__pkg__",` from `FROZEN` in `check-serial-compat-allowlist.py`.

Run: `bazel test --config=release //:legacy_flash_drain //:serial_compat_allowlist`
Expected: both FAIL — the bootmode operation still exists; the visibility list still names the bootmode UI package.

- [ ] **Step 2: Delete the legacy code**

```bash
git rm -r src/ui/desktop/flash/bootmode src/platform/desktop/common/flash/legacy/bootmode
```

In `mainwindow.cpp`, delete the `/* Unisia Jecs ECU Bootmode */` comment and both `_bootmode` `else if` branches. In `mainwindow.h`, delete the `// Bootmode` comment and the `flash_ecu_subaru_unisia_jecs_m32r_bootmode.h` include. In `src/ui/desktop/BUILD.bazel`, delete `"//src/ui/desktop/flash/bootmode",`. In `src/platform/desktop/common/serial/BUILD.bazel`, delete `"//src/ui/desktop/flash/bootmode:__pkg__",`.

- [ ] **Step 3: Trim the legacy package**

In `src/platform/desktop/common/flash/legacy/BUILD.bazel`:
- `srcs` becomes `["flash_operation_worker.cpp", "legacy_flash_utils.cpp"]`; `hdrs` becomes `["flash_operation_worker.h"]`.
- Replace the long `srcs` comment with:
  ```python
      # FlashOperationWorker (the shared QThread base) and legacy_flash_utils,
      # relocated from //src/backend/flash (step 5c, Task 15). Every family
      # subdirectory is gone: wave 7 drained bootmode/, the last one. 7b
      # deletes this package.
  ```
- Change the `normal_hdrs` comment to `# Not a Q_OBJECT header.`
- Delete the `"//src/algorithms/protocol/ssm/qt_compat",` dependency (its only user was the bootmode operation). Keep the rest; drop any other dependency only if `bazel build` then proves it unused.
- In `family_operation_sources`, add `allow_empty = True,` and replace the last sentence of its comment with: `Since wave 7 no family directory remains, so "*/*.cpp" matches nothing and allow_empty is True until 7b deletes the package.`

- [ ] **Step 4: Run everything**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //...`
Expected: PASS, including `//:legacy_flash_drain` ("OK: 0 families remaining, none added.") and `//:serial_compat_allowlist`.

- [ ] **Step 5: Commit**

```bash
git add -A src/ui/desktop src/platform/desktop/common/flash/legacy src/platform/desktop/common/serial/BUILD.bazel \
  scripts/check-legacy-flash-drain.py scripts/check-serial-compat-allowlist.py
git commit -m "feat(flash): delete the legacy Unisia Jecs M32R bootmode operation and dialog

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 11: 7a documentation and PR

**Files:**
- Create: `docs/unisia-jecs-m32r-bootmode-bench-checklist.md`
- Modify: `docs/flash-qualification-matrix.md:62`
- Modify: `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md` (status line, port item 4, new wave-7 note)
- Modify: `docs/superpowers/specs/2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md` (status line)
- Modify: `docs/modularization-plan.md:84-87, :222`

- [ ] **Step 1: Write the bench checklist**

```markdown
# Subaru Unisia Jecs M32R bootmode bench checklist

## 0. STOP — not hardware-qualified

Do not treat this family as proven until every section below has passed on
real hardware. Record the adapter (make, model, firmware; OpenPort 2.0 or
not), how VPP and MOD1 are supplied, the ECU part number and ECU ID, the
protocol selected, date, operator and result.

## 1. Read (`_20_bootmode` / `_30_bootmode`)

Bootmode Read is the [Unisia Jecs M32R K-Line](unisia-jecs-m32r-bench-checklist.md)
read. Run that checklist's section 1 once per bootmode protocol.

## 2. Kernel upload (attempt 1)

- Confirm the "Connect VPP and MOD1" prompt appears before any K-Line traffic,
  and that declining it sends nothing.
- Capture the upload at 39063 baud, even parity. Confirm each 128-byte chunk's
  local echo is drained on a direct serial adapter and that no bytes are lost.
- Record what, if anything, the ECU sends in the 200 ms after the upload.

## 3. Between attempts

- The executor drops both LEC lines when the upload ends, and the connection
  resets before programming. Legacy's port close did the same implicitly.
  Confirm the kernel is still running afterwards: attempt 2's `AF 31` must be
  answered by `EF 42`.
- Confirm Cancel on the "Remove MOD1" prompt stops before erase, and that the
  VPP notice follows.

## 4. Erase and program (attempt 2, bench ECU with a recovery path)

- Record how long `EF 42` and `EF 52` take against the 20 × 500 ms and
  20 × 1000 ms poll budgets.
- Provoke a negative reply (for example, no VPP) and record the frame.
  Confirm the error status sits in the byte after `EF`, as the executor
  assumes, and that `48` is reported as missing VPP.
- Record the exact bytes the ECU sends after the final `AF 69` block, or
  confirm it sends nothing. The executor accepts `EF 52` or silence.
- Read the ROM back and compare it with the written image.
- Close the dialog during programming. Confirm the VPP notice appears, without
  don't-power-off advice, and that power-cycling with MOD1 re-enters boot mode.
```

- [ ] **Step 2: Update the matrix row**

Replace the `FlashEcuSubaruUnisiaJecsM32rBootMode` row with:

```markdown
| FlashEcuSubaruUnisiaJecsM32rBootMode | bootmode | K-Line | read, write | yes | `subaru_unisia_jecs_m32r_kline_plan_test`, `subaru_unisia_jecs_m32r_kline_executor_test`, `subaru_unisia_jecs_m32r_bootmode_plan_test`, `subaru_unisia_jecs_m32r_bootmode_kernel_executor_test`, `subaru_unisia_jecs_m32r_bootmode_program_executor_test`, `test_flash_workflow`, `test_flash_dialog` @ Wave 7 | experimental | — | Exact pairs `sub_ecu_unisia_jecs_20_bootmode` / `M32R_128KB` and `_30_bootmode` / `M32R_256KB`; test_write is rejected, matching the cfg. Read is the `FlashEcuSubaruUnisiaJecsM32r` read, byte for byte. Write is two attempts: the cfg kernel (zero-padded to 128 bytes) uploaded at 39063 baud with even parity under VPP and MOD1, then erase and program at 19200 baud with VPP only, 128-byte `AF 61`/`AF 69` blocks sent as-is. **Operator flow change:** "remove MOD1" sits between the attempts and gains Cancel, which stops before erase; the VPP notice follows every write that started, not only success, and carries no don't-power-off advice because boot mode is always re-enterable. **Deliberate corrections:** cancellation reports cancelled instead of success; both erase polls fail when exhausted and on any other frame; the final `AF 69` reply is read and a malformed or negative reply fails; the image must be exactly the ROM size; LEC lines drop explicitly after each attempt; failure details name the documented status codes. Silence after `AF 69` is accepted with a warning; see the [bench checklist](unisia-jecs-m32r-bootmode-bench-checklist.md). No hardware qualification is claimed. |
```

In the `FlashEcuSubaruUnisiaJecsM32r` row, replace "The `_bootmode` protocols stay on the legacy path." with "The `_bootmode` protocols read through this family since wave 7."

- [ ] **Step 3: Update the wave-6 spec, this spec, and the modularization plan**

Wave-6 spec status line:

```markdown
**Status:** complete — waves 6a-1 through 6c-3 are merged. The last legacy family migrated in wave 7.
```

Wave-6 spec port item 4 — append to its paragraph:

```markdown
**Amended by wave 7:** only `enable_boot_mode_lines()` landed. Bootmode's
write became two attempts split where legacy already reset the connection,
so each attempt configures its own parity and mid-session `set_parity()` has
no caller. See the [wave 7 design](2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md).
```

Wave-6 spec — append after the 6c-3 note:

```markdown
## Wave 7 note

The [wave 7 design](2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md)
migrates `FlashEcuSubaruUnisiaJecsM32rBootMode`: Read joins the 6c-3 family,
Write is two attempts around a `RemoveMod1` prompt, and port item 4 lands as
`enable_boot_mode_lines()` alone. The drain moves from one entry to none.
```

Wave-7 spec status line: `**Status:** implemented in PR 7a; 7b (teardown) follows.`

Modularization plan: change the 6c-3 line to `— merged (#359).` and add after it:

```markdown
- Wave 7 `FlashEcuSubaruUnisiaJecsM32rBootMode` — implemented on this branch
  (7a); legacy package teardown follows in 7b. See the
  [family design](superpowers/specs/2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md).
```

and change line 222's heading suffix to `flash-tail Wave 7 implemented on this branch, with 0 legacy families remaining`.

- [ ] **Step 4: Verify the whole PR**

Run:
```bash
prek run --all-files
bazel test --config=release //...
bazel run //:clang_tidy_report_changed
```
Expected: all green; clang-tidy reports nothing in changed files. Fix findings in the task that owns the file, then re-run.

- [ ] **Step 5: Commit, push, open the PR**

```bash
git add docs
git commit -m "docs(flash): record the Unisia Jecs M32R bootmode migration (wave 7a)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
git push -u origin feat/wave7-unisia-jecs-m32r-bootmode
gh pr create --base master --title "feat(flash): migrate Subaru Unisia Jecs M32R bootmode (wave 7a)" --body "$(cat <<'EOF'
## Summary
- Bootmode Read joins the 6c-3 Unisia Jecs M32R K-Line family.
- Bootmode Write is two portable attempts — kernel upload, then erase and program — around a RemoveMod1 prompt.
- `IKlineFlashTransport::enable_boot_mode_lines()` (wave-6 port item 4, reduced).
- Legacy bootmode operation, dialog and MainWindow branches deleted; `REMAINING` is empty.

Design: docs/superpowers/specs/2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md

## Test plan
- [ ] `bazel test --config=release //...`
- [ ] `prek run --all-files`
- [ ] `bazel run //:clang_tidy_report_changed`
- [ ] Hardware: not qualified; see docs/unisia-jecs-m32r-bootmode-bench-checklist.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc
EOF
)"
```

Push and PR creation are outward-facing: confirm with the user before running them.

---

# PR 7b — legacy teardown and wave close

Start only after 7a is merged:

```bash
git switch master && git pull --ff-only && git switch -c feat/wave7b-legacy-flash-teardown
```

### Task 12: Delete the legacy flash package and the drain ratchet

**Files:**
- Delete: `src/platform/desktop/common/flash/legacy/` (7 files)
- Delete: `scripts/check-legacy-flash-drain.py`
- Modify: `BUILD.bazel:68-74` (root), `scripts/check-serial-compat-allowlist.py:24-26`, `src/platform/desktop/common/serial/BUILD.bazel` (visibility)

- [ ] **Step 1: Prove there are no consumers**

Run:
```bash
grep -rn "common/flash/legacy\|legacy_flash_operations\|flash_operation_worker\|legacy_flash_utils\|FlashOperationWorker\|legacy_flash_drain\|check-legacy-flash-drain" \
  --exclude-dir=.git --exclude-dir=.worktrees --exclude-dir='bazel-*' --exclude-dir=docs .
```
Expected: hits only inside `src/platform/desktop/common/flash/legacy/`, the root `BUILD.bazel` `legacy_flash_drain` rule, the script itself, the serial `BUILD.bazel` visibility entry, and `check-serial-compat-allowlist.py`. Any other hit is a consumer: stop and report it.

- [ ] **Step 2: Shrink the allowlist and watch it fail**

Remove `"//src/platform/desktop/common/flash/legacy:__pkg__",` and its two-line comment from `FROZEN`.
Run: `bazel test --config=release //:serial_compat_allowlist`
Expected: FAIL — the serial visibility list still names the legacy package.

- [ ] **Step 3: Delete**

```bash
git rm -r src/platform/desktop/common/flash/legacy scripts/check-legacy-flash-drain.py
```

Delete the `py_test(name = "legacy_flash_drain", …)` rule from the root `BUILD.bazel` and `"//src/platform/desktop/common/flash/legacy:__pkg__",` from the serial `BUILD.bazel` visibility list.

- [ ] **Step 4: Run everything**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //...`
Expected: PASS, including `//:serial_compat_allowlist`.

- [ ] **Step 5: Commit**

```bash
git add -A BUILD.bazel scripts src/platform/desktop/common
git commit -m "feat(flash): delete the drained legacy flash package and its ratchet

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 13: Delete `ssm:qt_compat`

**Files:**
- Delete: `src/algorithms/protocol/ssm/qt_compat/` (4 files)
- Modify: `bazel/qt/BUILD.bazel:17`

- [ ] **Step 1: Prove there are no consumers**

Run:
```bash
grep -rn "ssm/qt_compat" --exclude-dir=.git --exclude-dir=.worktrees --exclude-dir='bazel-*' --exclude-dir=docs .
```
Expected: hits only inside `src/algorithms/protocol/ssm/qt_compat/` and `bazel/qt/BUILD.bazel`.

- [ ] **Step 2: Delete**

```bash
git rm -r src/algorithms/protocol/ssm/qt_compat
```

Delete `"//src/algorithms/protocol/ssm/qt_compat",` from `qt_layer` in `bazel/qt/BUILD.bazel`.

- [ ] **Step 3: Run everything**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //...`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add -A bazel/qt/BUILD.bazel src/algorithms/protocol/ssm
git commit -m "refactor(protocol): delete the unused SSM Qt compatibility shim

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

---

### Task 14: Close the wave in the docs, verify the completion criterion, open 7b

**Files:**
- Modify: `docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md`
- Modify: `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md`
- Modify: `docs/superpowers/specs/2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md`
- Modify: `docs/modularization-plan.md`, `docs/tech-debt.md`, `docs/flash-qualification-matrix.md:64, :89-108`
- Modify: any other doc the Step 1 grep finds

- [ ] **Step 1: Find stale references**

Run:
```bash
grep -rn "flash/legacy\|legacy_flash_drain\|check-legacy-flash-drain\|ssm:qt_compat\|ssm/qt_compat\|FlashOperationWorker" docs CLAUDE.md
```
Every hit that describes the present tense (not history in a dated spec's body) gets updated in the steps below.

- [ ] **Step 2: Tail design**

Add a status line under the title: `**Status:** complete — wave 7 merged; the completion criterion holds.` In the wave-6 row's rationale, add a sentence: `Wave 6 found these families call write_serial_data_echo_check, which IKlineFlashTransport::write() already is; no new shared port was needed, and the calls on no port fell from six to two, then to one in wave 7.` After "Completion criterion", add:

```markdown
**Met in wave 7** ([design](2026-09-25-step5-tail-wave7-unisia-jecs-m32r-bootmode-design.md)):
`REMAINING` emptied in 7a; 7b deleted the package, the ratchet, the allowlist
entry, and `ssm:qt_compat`. `protocol:qt_compat` remains by ADR 0004.
```

- [ ] **Step 3: Other docs**

- Wave-7 spec status: `**Status:** complete — 7a and 7b merged.`
- Modularization plan: the wave-7 line becomes merged with both PR numbers; step 5's heading becomes `complete — the flash tail drained in wave 7`.
- `tech-debt.md` P1 flash-orchestration section: state that every flash family runs through `FlashWorkflowFactory` and the common `FlashDialog`, `FlashOperationWorker` is gone, and remove any bullet that asks to migrate families off `SerialPortActions`. Remove the `ssm:qt_compat` mention from the shim discussion.
- Matrix: the notes at `:64`, `:89-94` and `:108` stop naming `legacy/{…}` directories; say the legacy package was deleted in wave 7.

- [ ] **Step 4: Check the completion criterion line by line**

Run:
```bash
test ! -e src/platform/desktop/common/flash/legacy && echo "legacy package gone"
test ! -e scripts/check-legacy-flash-drain.py && echo "ratchet gone"
grep -c "flash/legacy" scripts/check-serial-compat-allowlist.py   # expect 0
test ! -e src/algorithms/protocol/ssm/qt_compat && echo "ssm shim gone"
ls src/algorithms/protocol/colt/qt_compat src/algorithms/protocol/mut_dma/qt_compat src/algorithms/crypto/qt_compat src/algorithms/expression/qt_compat 2>&1 | grep -c "No such file"   # expect 4
grep -E "^\| Flash" docs/flash-qualification-matrix.md | grep -vc "| yes |"   # expect 0 rows not portable
```
Expected: the four `echo` lines print; counts are `0`, `4`, `0`. Paste the output into the PR body.

- [ ] **Step 5: Verify, commit, push, open the PR**

Run:
```bash
prek run --all-files
bazel test --config=release //...
```
Expected: green.

```bash
git add docs CLAUDE.md
git commit -m "docs(flash): close the step 5 flash drain after wave 7

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
git push -u origin feat/wave7b-legacy-flash-teardown
gh pr create --base master --title "feat(flash): delete the legacy flash package (wave 7b)" --body "$(cat <<'EOF'
## Summary
- Deletes `src/platform/desktop/common/flash/legacy/`, the `//:legacy_flash_drain` ratchet and its allowlist entry.
- Deletes `//src/algorithms/protocol/ssm/qt_compat`.
- Closes the wave-6 and flash-tail docs; step 5's flash drain is complete.

## Completion criterion
<paste Task 14 Step 4 output>

## Test plan
- [ ] `bazel test --config=release //...`
- [ ] `prek run --all-files`

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc
EOF
)"
```

Push and PR creation are outward-facing: confirm with the user before running them.
