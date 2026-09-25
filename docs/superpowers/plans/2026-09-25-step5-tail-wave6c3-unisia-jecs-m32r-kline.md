# Wave 6c-3 — Subaru Unisia Jecs M32R K-Line — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate `FlashEcuSubaruUnisiaJecsM32rOperation` to a portable plan and `IKlineFlashExecutor` routed through `FlashWorkflow`, taking `//:legacy_flash_drain` from two entries to one.

**Architecture:** One family plan covers the four exact `(protocol, mcu)` pairs `sub_ecu_unisia_jecs_{20,30,40,70}` through a variant table; `_40` and `_70` are read-only. The executor speaks framed SSM over `write()` / `read()`: Read is a 38400-baud `A0` block read from `0x100000`, with a cold 4800-baud init when needed; Write enters flash mode, raises programming voltage, erases, and programs 128-byte XOR-`0x82` blocks, gating every reply and dropping the LEC lines on every exit. A new desktop workflow collects `ApplyProgrammingVoltage` before the attempt and shows `RemoveProgrammingVoltage` after it; the legacy dialog package, operation and `MainWindow` branches are deleted.

**Tech Stack:** C++23, Bazel, GoogleTest (`fastecu_portable_gtest`), QtTest for the desktop workflow suite.

**Spec:** [Wave 6c-3 — Subaru Unisia Jecs M32R K-Line — Design](../specs/2026-09-25-step5-tail-wave6c3-unisia-jecs-m32r-kline-design.md). Parent: [Wave 6 singletons](../specs/2026-09-19-step5-tail-wave6-singletons-design.md).

## Global Constraints

- Behavior-correction policy is the 6a-3 / 6a-4 / 6b-2 / 6c-1 exception. Command bytes, sequencing and timing budgets are preserved. Reply-gating, integrity and cancellation defects are corrected, and each correction is named in the matrix notes.
- One PR. No port method is added: every call maps onto the existing `IKlineFlashTransport`.
- The executor calls `write()` / `read()` for traffic, never `write_raw()` / `read_raw()`. It never calls `configure()`, `open()`, `close()` or `reset_connection()` (ADR 0015). The ISO-14230 header is cleared in `before_transport_configure()`.
- Tester ID `0xF0`, target ID `0x10`. Requests are `80 10 F0 <len> <payload> <sum8>`; replies are accepted only through `SsmProtocol::hasValidFrame(frame, 0xF0, 0x10)`.
- Once `execute()` begins a Write, `disable_lec_lines()` runs on every exit path. A cleanup failure turns an otherwise successful run into a failure and never replaces an earlier error. Read never touches the LEC lines.
- Backend operations return `fastecu::Result<T>`, checked with `.has_value()`, never implicit `operator bool`. Exceptions never cross a port. Do not add an `ErrorKind`.
- Pure protocol code uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView`. The backend never sees `EcuCalDefStructure`, Qt, threads or the filesystem.
- New backend targets live in `src/backend/flash/ecu/` and are registered by name under `"src/backend/flash/ecu"` in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`).
- Ratchets only shrink. Remove exactly `ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp` from `REMAINING` and exactly `//src/ui/desktop/flash/ecu:__pkg__` from the `serial_qt_compat` visibility list and its `FROZEN` copy. Add no entry anywhere.
- Executor steps carry a comment citing the legacy function and line in `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp`. That file exists until Task 4 deletes it; read the cited lines while it does.
- Every test that pins a correction is mutation-checked: change the production line, run the named test, watch it fail, then restore the file byte-identically (`git diff --exit-code <file>`). The mutations are listed in the Task 2 and Task 3 steps.
- Tests are package-owned and co-located. Mocks and fakes stay package-owned.
- Markdown cross-references are links with human-readable text, not backticked paths.
- Work lands through pull requests. Do not push or open a PR without explicit user approval.

## Review Focus

- **Operator closes the dialog during the erase poll.** Expect `Cancelled`, no further command, and the programming-voltage line dropped. Pinned by `CancellationDuringTheErasePollDropsTheLine` (Task 3).
- **Adapter disconnects mid-programming.** Expect the transport error to propagate unchanged and the line dropped. Pinned by `TransportErrorMidProgrammingPropagatesAndDropsTheLine` (Task 3).
- **The read-mode probe answers with a valid but truncated `FF` frame.** Expect it treated as "not in read mode" and a cold init to follow, not a failure or a wrong ECU ID. Pinned by `TruncatedProbeReplyFallsBackToColdInit` (Task 2).
- **A Read never raises or drops the programming-voltage line.** Legacy read touched no LEC line. Pinned by the `control_line_trace_` assertion in `ReadsTheRomWhenAlreadyInReadMode` (Task 2).
- **No adapter information (null serial) defaults to prompting for VPP.** Prompting is the safe default. Pinned by `unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter` (Task 4), which runs with a null serial.

## Legacy constants

| Name | Value | Legacy source |
|---|---|---|
| initial baud | 4800 | `execute()` :57 |
| read baud | 38400 | `read_mem()` :102, :195 |
| write baud | 19200 | `write_mem()` :347, :431 |
| `serial_read_timeout` | 2000 ms | header; `BF`, `B8`, `AF`, `AF 11` |
| `serial_read_medium_timeout` | 500 ms | header; erase polls, trailing read |
| `serial_read_extra_long_timeout` | 3000 ms | header; `A0` pages, `AF 61` blocks |
| read base | `0x100000` | `read_mem()` :219 |
| page / block | `0x80` | `read_mem()` :220, `write_mem()` :523 |
| inter-page pacing | 1 ms | `read_mem()` :314 |
| erase-start poll | 20 rounds × (500 ms read, 500 ms sleep) for `EF 42` | `write_mem()` :448-476 |
| erase-done poll | 40 rounds × (500 ms read, 500 ms sleep) for `EF 52` | `write_mem()` :488-516 |
| block XOR | `0x82` | `write_mem()` :525, :557 |
| ECU ID | frame bytes 8–12 | `read_mem()` :162-163 |

## File map

- Create `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_types.h`
- Create `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.{h,cpp}` + `_plan_test.cpp`
- Create `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.{h,cpp}` + `_executor_test.cpp`
- Modify `src/backend/flash/ecu/BUILD.bazel`, `src/backend/flash/BUILD.bazel`, `src/backend/flash/flash_types.h`, `src/backend/flash/flash_plan.cpp`, `src/backend/flash/testing/flash_printers.h`, `src/backend/flash/flash_validation_test.cpp`, `bazel/portable_targets.bzl`
- Modify `src/platform/desktop/common/flash/flash_workflow.{h,cpp}`, `flash_workflow_test.cpp`, `src/platform/desktop/common/flash/BUILD.bazel`
- Modify `src/ui/desktop/flash/common/flash_dialog.cpp`, `src/ui/desktop/mainwindow.{h,cpp}`, `src/ui/desktop/BUILD.bazel`
- Modify `src/platform/desktop/common/flash/legacy/BUILD.bazel`, `src/platform/desktop/common/serial/BUILD.bazel`, `scripts/check-legacy-flash-drain.py`, `scripts/check-serial-compat-allowlist.py`
- Delete `src/ui/desktop/flash/ecu/` and `src/platform/desktop/common/flash/legacy/ecu/`
- Modify `docs/flash-qualification-matrix.md`, `docs/modularization-plan.md`, both specs; create `docs/unisia-jecs-m32r-bench-checklist.md`

**Branch.** The branch `docs/wave6c3-unisia-jecs-m32r-design` carries the spec and this plan, based on `a90bb178`. Before Task 1, rename it:

```bash
git branch -m docs/wave6c3-unisia-jecs-m32r-design feat/wave6c3-unisia-jecs-m32r-kline
```

---

### Task 1: Plan, family registration, validation

**Files:**
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_types.h`, `subaru_unisia_jecs_m32r_kline_plan.h`, `subaru_unisia_jecs_m32r_kline_plan.cpp`, `subaru_unisia_jecs_m32r_kline_plan_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `src/backend/flash/BUILD.bazel:40`, `src/backend/flash/flash_types.h`, `src/backend/flash/flash_plan.cpp:60-61`, `src/backend/flash/testing/flash_printers.h:112-114`, `src/backend/flash/flash_validation_test.cpp:221-222`, `bazel/portable_targets.bzl:123`

**Interfaces:**
- Produces: `struct SubaruUnisiaJecsM32rKlinePlan { int initial_baud; std::uint8_t tester_id; std::uint8_t target_id; };`, `FlashFamily::SubaruUnisiaJecsM32rKline`, `ConfirmationSpec::Id::ApplyProgrammingVoltage`,
  `Result<FlashPlan> build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name, std::string_view mcu_type, std::optional<bytes::Bytes> image, bool adapter_supplies_programming_voltage);`,
  `Status validate_subaru_unisia_jecs_m32r_kline_plan(const FlashPlan& plan);`
- Plan shape: family plan `{4800, 0xF0, 0x10}`; Read → transfer `{0x100000, rom_size}`, no image, no confirmations. Write (`_20`, `_30` only) → transfer `{0, rom_size}`, image exactly `rom_size` bytes, confirmations `{ApplyProgrammingVoltage}` when the adapter does not supply VPP and empty otherwise. Both → no kernel, no erase regions. TestWrite → `Unsupported`; Write on `_40` / `_70` → `Unsupported`.

- [ ] **Step 1: Register the family and the confirmation id so tests can compile**

`src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_types.h`:

```cpp
#pragma once

#include <cstdint>

namespace fastecu::flash
{
// Step 5 tail, wave 6c-3. Framed SSM over plain K-Line; the ROM size comes
// from the plan's transfer region, so only the session parameters live here.
struct SubaruUnisiaJecsM32rKlinePlan
{
    int initial_baud;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};
} // namespace fastecu::flash
```

In `src/backend/flash/flash_types.h`:
- add `#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_types.h"` after the `subaru_unisia_jecs_types.h` include;
- append to `enum class FlashFamily`, after `SubaruDensoMc68hc16y5_02Bdm,`:
  ```cpp
      // Step 5 tail, wave 6c-3.
      SubaruUnisiaJecsM32rKline,
  ```
- in `ConfirmationSpec::Id`, after `TopRegionBootstrap,` add:
  ```cpp
          // Step 5 tail, wave 6c-3. Same contract as the two above: the
          // operator confirmed, before the executor started, that external
          // programming voltage is applied because the adapter cannot supply
          // it.
          ApplyProgrammingVoltage,
  ```
- append `, SubaruUnisiaJecsM32rKlinePlan` as the last `FamilyPlan` alternative (after `SubaruDensoMc68hc16y5_02BdmPlan`);
- after the `FamilyTraits<SubaruDensoMc68hc16y5_02BdmPlan>` specialization add:
  ```cpp
  template <> struct FamilyTraits<SubaruUnisiaJecsM32rKlinePlan>
  {
      static constexpr FlashFamily family = FlashFamily::SubaruUnisiaJecsM32rKline;
      static constexpr TransportKind transport = TransportKind::Kline;
  };
  ```
- after `family_requires_kernel_v<SubaruDensoMc68hc16y5_02BdmPlan> = false;` add:
  ```cpp
  // Step 5 tail, wave 6c-3. The ECU's own boot ROM handles flash mode; no
  // kernel is uploaded.
  template <> inline constexpr bool family_requires_kernel_v<SubaruUnisiaJecsM32rKlinePlan> = false;
  ```

In `src/backend/flash/flash_plan.cpp`, after the `SubaruDensoMc68hc16y5_02Bdm` case:
```cpp
    case FlashFamily::SubaruUnisiaJecsM32rKline:
        return "SubaruUnisiaJecsM32rKline";
```

In `src/backend/flash/testing/flash_printers.h`, after the `SubaruDensoMc68hc16y5_02Bdm` case:
```cpp
    case FlashFamily::SubaruUnisiaJecsM32rKline:
        *os << "SubaruUnisiaJecsM32rKline";
        return;
```

In `src/backend/flash/BUILD.bazel`, after `"//src/backend/flash/ecu:subaru_unisia_jecs_types",` add `"//src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_types",`. If buildifier re-sorts the list, accept its order.

In `src/backend/flash/flash_validation_test.cpp`, append to the family table after the `SubaruDensoMc68hc16y5_02Bdm` row:
```cpp
        {FlashFamily::SubaruUnisiaJecsM32rKline, TransportKind::Kline,
         SubaruUnisiaJecsM32rKlinePlan{.initial_baud = 4800, .tester_id = 0xf0, .target_id = 0x10},
         "SubaruUnisiaJecsM32rKline"},
```

- [ ] **Step 2: Write the failing plan tests**

`src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h`:

```cpp
#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
// Exact protocol/MCU pairs sub_ecu_unisia_jecs_{20,30,40,70}; _40 and _70 are
// read-only. A Write plan carries ApplyProgrammingVoltage unless
// `adapter_supplies_programming_voltage`; the desktop workflow collects that
// confirmation before the executor starts.
Result<FlashPlan> build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                           bool adapter_supplies_programming_voltage);
Status validate_subaru_unisia_jecs_m32r_kline_plan(const FlashPlan& plan);
} // namespace fastecu::flash
```

`src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

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
    bool writable;
};

constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000, true},
    {"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000, true},
    {"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000, false},
    {"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000, false},
});

constexpr SubaruUnisiaJecsM32rKlinePlan kWire{.initial_baud = 4800, .tester_id = 0xf0, .target_id = 0x10};

FlashPlanFields read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruUnisiaJecsM32rKline,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_unisia_jecs_20",
        .mcu_name = "M32R_128KB",
        .transfer_region = MemoryRegion{0x100000, 0x20000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = kWire,
        .confirmations = {},
    };
}

FlashPlanFields write_fields()
{
    FlashPlanFields fields = read_fields();
    fields.operation = FlashOperation::Write;
    fields.transfer_region = MemoryRegion{0, 0x20000};
    fields.image = bytes::Bytes(0x20000, 0xab);
    return fields;
}

TEST(SubaruUnisiaJecsM32rKlinePlan, ReadCoversEachVariantFromTheReadBase)
{
    for (const Variant& variant : kVariants)
    {
        const auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, variant.protocol,
                                                                   variant.mcu, std::nullopt, false);
        ASSERT_THAT(plan, IsOk()) << variant.protocol;
        EXPECT_EQ(plan->family(), FlashFamily::SubaruUnisiaJecsM32rKline);
        EXPECT_EQ(plan->transport(), TransportKind::Kline);
        EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0x100000, variant.rom_size})) << variant.protocol;
        EXPECT_FALSE(plan->image().has_value());
        EXPECT_FALSE(plan->kernel().has_value());
        EXPECT_TRUE(plan->confirmations().empty()) << "Read never raises programming voltage";
        const auto& wire = std::get<SubaruUnisiaJecsM32rKlinePlan>(plan->family_plan());
        EXPECT_EQ(wire.initial_baud, 4800);
        EXPECT_EQ(wire.tester_id, 0xf0);
        EXPECT_EQ(wire.target_id, 0x10);
    }
}

TEST(SubaruUnisiaJecsM32rKlinePlan, WriteCarriesTheImageAndAsksForVppOnlyWithoutAdapterSupply)
{
    for (const Variant& variant : kVariants)
    {
        if (!variant.writable)
        {
            continue;
        }
        const auto prompted = build_subaru_unisia_jecs_m32r_kline_plan(
            FlashOperation::Write, variant.protocol, variant.mcu, bytes::Bytes(variant.rom_size, 0x5a), false);
        ASSERT_THAT(prompted, IsOk()) << variant.protocol;
        EXPECT_EQ(prompted->transfer_region(), (MemoryRegion{0, variant.rom_size}));
        EXPECT_EQ(prompted->image(), std::optional<bytes::Bytes>(bytes::Bytes(variant.rom_size, 0x5a)));
        ASSERT_EQ(prompted->confirmations().size(), 1U);
        EXPECT_EQ(prompted->confirmations()[0].id, ConfirmationSpec::Id::ApplyProgrammingVoltage);

        const auto supplied = build_subaru_unisia_jecs_m32r_kline_plan(
            FlashOperation::Write, variant.protocol, variant.mcu, bytes::Bytes(variant.rom_size, 0x5a), true);
        ASSERT_THAT(supplied, IsOk()) << variant.protocol;
        EXPECT_TRUE(supplied->confirmations().empty());
    }
}

TEST(SubaruUnisiaJecsM32rKlinePlan, RejectsEveryOtherIdentity)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_unisia_jecs_20", "M32R_256KB"},
             {"sub_ecu_unisia_jecs_70", "M32R_512KB_1block"},
             {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB"},
             {"sub_ecu_unisia_jecs_30x", "M32R_256KB"},
             {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
         }))
    {
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, protocol, mcu, std::nullopt, false),
                    IsErr(ErrorKind::InvalidConfig))
            << protocol << " / " << mcu;
    }
}

TEST(SubaruUnisiaJecsM32rKlinePlan, RejectsTestWriteEverywhereAndWriteOnReadOnlyVariants)
{
    for (const Variant& variant : kVariants)
    {
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::TestWrite, variant.protocol, variant.mcu,
                                                             bytes::Bytes(variant.rom_size, 0x00), false),
                    IsErr(ErrorKind::Unsupported))
            << variant.protocol;
        if (!variant.writable)
        {
            EXPECT_THAT(build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Write, variant.protocol, variant.mcu,
                                                                 bytes::Bytes(variant.rom_size, 0x00), false),
                        IsErr(ErrorKind::Unsupported))
                << variant.protocol;
        }
    }
}

TEST(SubaruUnisiaJecsM32rKlinePlan, WriteImageMustBeExactlyTheRomSize)
{
    constexpr std::string_view protocol = "sub_ecu_unisia_jecs_20";
    constexpr std::string_view mcu = "M32R_128KB";
    // Legacy write_mem() :524 computed blocks = size / 0x80 and silently
    // dropped any tail; one byte over is exactly that trailing partial block.
    for (const std::size_t size : {std::size_t{0x20000 - 1}, std::size_t{0x20000 + 1}, std::size_t{0x10000}})
    {
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Write, protocol, mcu,
                                                             bytes::Bytes(size, 0x00), false),
                    IsErr(ErrorKind::InvalidConfig))
            << size;
    }
    EXPECT_THAT(build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Write, protocol, mcu, std::nullopt, false),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, protocol, mcu,
                                                         bytes::Bytes(0x20000, 0x00), false),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruUnisiaJecsM32rKlinePlan, ValidatorRejectsHandBuiltShapes)
{
    struct Case
    {
        const char *name;
        FlashPlanFields fields;
        ErrorKind expected;
    };
    std::vector<Case> cases;
    {
        auto fields = read_fields();
        fields.family_plan = SubaruUnisiaJecsM32rKlinePlan{.initial_baud = 9600, .tester_id = 0xf0, .target_id = 0x10};
        cases.push_back({"wrong baud", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.family_plan = SubaruUnisiaJecsM32rKlinePlan{.initial_baud = 4800, .tester_id = 0xf1, .target_id = 0x10};
        cases.push_back({"wrong tester id", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.transfer_region = MemoryRegion{0, 0x20000};
        cases.push_back({"read region at flash address", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::ApplyProgrammingVoltage, {}}};
        cases.push_back({"read with VPP confirmation", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.kernel = KernelImage{.id = "k", .load_address = 0, .bytes = bytes::Bytes{1}};
        cases.push_back({"kernel", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields();
        fields.transfer_region = MemoryRegion{0x100000, 0x20000};
        cases.push_back({"write region at read address", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields();
        fields.image = bytes::Bytes(0x1ff80, 0xab);
        cases.push_back({"short image", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields();
        fields.erase_regions = {MemoryRegion{0, 0x20000}};
        cases.push_back({"erase regions", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields();
        fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::EraseTrigger, {}}};
        cases.push_back({"foreign confirmation", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields();
        fields.operation = FlashOperation::TestWrite;
        cases.push_back({"test write", std::move(fields), ErrorKind::Unsupported});
    }
    {
        auto fields = write_fields();
        fields.target_id = "sub_ecu_unisia_jecs_40";
        fields.mcu_name = "M32R_384KB";
        fields.transfer_region = MemoryRegion{0, 0x60000};
        fields.image = bytes::Bytes(0x60000, 0xab);
        cases.push_back({"write on read-only variant", std::move(fields), ErrorKind::Unsupported});
    }

    for (auto& test_case : cases)
    {
        auto plan = validate_and_build(std::move(test_case.fields));
        ASSERT_THAT(plan, IsOk()) << test_case.name;
        EXPECT_THAT(validate_subaru_unisia_jecs_m32r_kline_plan(*plan), IsErr(test_case.expected)) << test_case.name;
    }
}
} // namespace
} // namespace fastecu::flash
```

Add to `src/backend/flash/ecu/BUILD.bazel`, after the `subaru_denso_mc68hc16y5_02_bdm_executor_test` target:

```starlark
cc_library(
    name = "subaru_unisia_jecs_m32r_kline_types",
    hdrs = ["subaru_unisia_jecs_m32r_kline_types.h"],
)

cc_library(
    name = "subaru_unisia_jecs_m32r_kline_plan",
    srcs = ["subaru_unisia_jecs_m32r_kline_plan.cpp"],
    hdrs = ["subaru_unisia_jecs_m32r_kline_plan.h"],
    deps = [
        ":subaru_unisia_jecs_m32r_kline_types",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/flash:flash_plan",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_unisia_jecs_m32r_kline_plan_test",
    srcs = ["subaru_unisia_jecs_m32r_kline_plan_test.cpp"],
    deps = [
        ":subaru_unisia_jecs_m32r_kline_plan",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

In `bazel/portable_targets.bzl`, under `"src/backend/flash/ecu"`, after `"subaru_denso_mc68hc16y5_02_bdm_executor",` add:
```starlark
        "subaru_unisia_jecs_m32r_kline_types",
        "subaru_unisia_jecs_m32r_kline_plan",
```

Create `subaru_unisia_jecs_m32r_kline_plan.cpp` holding only its own include, so the target links and the tests fail on missing symbols:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
```

- [ ] **Step 3: Run to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_plan_test`
Expected: FAIL to link, with undefined `build_subaru_unisia_jecs_m32r_kline_plan` / `validate_subaru_unisia_jecs_m32r_kline_plan`.

- [ ] **Step 4: Implement the plan**

Replace `subaru_unisia_jecs_m32r_kline_plan.cpp` with:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
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
    bool writable;
};

// protocols.cfg: _20 and _30 are read/write; _40 and _70 are read-only.
// test_write is "no" for all four.
constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000, true},
    {"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000, true},
    {"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000, false},
    {"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000, false},
});
// Legacy read_mem() :219 reads fblocks[0].start + 0x100000, and every M32R
// variant above starts at 0; write_mem() :522 programs from flash address 0.
constexpr std::uint32_t kReadBase = 0x100000;
// Legacy execute() :55-57.
constexpr SubaruUnisiaJecsM32rKlinePlan kWire{.initial_baud = 4800, .tester_id = 0xf0, .target_id = 0x10};

Result<Variant> find_variant(std::string_view protocol, std::string_view mcu)
{
    const auto variant = std::ranges::find(kVariants, protocol, &Variant::protocol);
    if (variant == kVariants.end() || variant->mcu != mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Unisia Jecs M32R protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const flashdev_t *device = find_flash_device(mcu);
    if (device == nullptr || device->romsize != variant->rom_size || device->fblocks == nullptr ||
        device->fblocks[0].start != 0)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R memory map is invalid");
    }
    return *variant;
}

Status check_operation(const Variant& variant, FlashOperation operation)
{
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "Unisia Jecs M32R has no test write");
    }
    if (operation == FlashOperation::Write && !variant.writable)
    {
        return fail(ErrorKind::Unsupported, std::format("{} is read-only", variant.protocol));
    }
    return {};
}

MemoryRegion region_for(const Variant& variant, FlashOperation operation)
{
    return operation == FlashOperation::Read ? MemoryRegion{kReadBase, variant.rom_size}
                                             : MemoryRegion{0, variant.rom_size};
}
} // namespace

Status validate_subaru_unisia_jecs_m32r_kline_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruUnisiaJecsM32rKline || plan.transport() != TransportKind::Kline)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Unisia Jecs M32R");
    }
    const auto variant = find_variant(plan.target_id(), plan.mcu_name());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status operation = check_operation(*variant, plan.operation()); !operation.has_value())
    {
        return operation;
    }
    const auto *wire = std::get_if<SubaruUnisiaJecsM32rKlinePlan>(&plan.family_plan());
    if (wire == nullptr || wire->initial_baud != kWire.initial_baud || wire->tester_id != kWire.tester_id ||
        wire->target_id != kWire.target_id)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R wire parameters are invalid");
    }
    if (!plan.erase_regions().empty() || plan.kernel().has_value() ||
        plan.transfer_region() != region_for(*variant, plan.operation()))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R plan shape is invalid");
    }
    const auto& confirmations = plan.confirmations();
    if (plan.operation() == FlashOperation::Read)
    {
        if (plan.image().has_value() || !confirmations.empty())
        {
            return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R read-plan shape is invalid");
        }
        return {};
    }
    if (!plan.image().has_value() || plan.image()->size() != variant->rom_size)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R write image must be exactly the ROM size");
    }
    if (confirmations.size() > 1 ||
        (confirmations.size() == 1 && confirmations[0].id != ConfirmationSpec::Id::ApplyProgrammingVoltage))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R write confirmations are invalid");
    }
    return {};
}

Result<FlashPlan> build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                           bool adapter_supplies_programming_voltage)
{
    const auto variant = find_variant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status checked = check_operation(*variant, operation); !checked.has_value())
    {
        return std::unexpected(checked.error());
    }
    if (operation == FlashOperation::Read)
    {
        if (image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != variant->rom_size)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} write image must be exactly {} bytes, not {}", variant->protocol,
                                variant->rom_size, image.has_value() ? image->size() : 0));
    }

    std::vector<ConfirmationSpec> confirmations;
    if (operation == FlashOperation::Write && !adapter_supplies_programming_voltage)
    {
        confirmations.push_back(ConfirmationSpec{ConfirmationSpec::Id::ApplyProgrammingVoltage, {}});
    }

    auto plan = validate_and_build(FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::SubaruUnisiaJecsM32rKline,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = region_for(*variant, operation),
        .erase_regions = {},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = kWire,
        .confirmations = std::move(confirmations),
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_unisia_jecs_m32r_kline_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
```

- [ ] **Step 5: Run to verify they pass**

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_plan_test //src/backend/flash:all
bazel build --config=release //:portable_closure
```
Expected: PASS. `//src/backend/flash:all` includes `flash_validation_test`, which exercises the new table row.

- [ ] **Step 6: Commit**

```bash
git add bazel/portable_targets.bzl src/backend/flash
git commit -m "feat(flash): add the Unisia Jecs M32R K-Line plan"
```

---

### Task 2: Executor — setup, SSM helpers, Read

**Files:**
- Create: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h`, `subaru_unisia_jecs_m32r_kline_executor.cpp`, `subaru_unisia_jecs_m32r_kline_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `build_subaru_unisia_jecs_m32r_kline_plan`, `validate_subaru_unisia_jecs_m32r_kline_plan` (Task 1); `SsmProtocol::addHeader`, `SsmProtocol::hasValidFrame`; `non_iso14230_kline_config_from`, `check_family`; `ScriptedKlineFlashTransport::exchange`, `expectWrite`, `queueRead`, `queue_no_frame`, `queue_error`, `baud_calls_`, `header_mode_calls_`, `control_line_trace_`, `read_timeouts_`, `writesConsumed()`, `scriptConsumed()`.
- Produces: `class SubaruUnisiaJecsM32rKlineExecutor final : public IKlineFlashExecutor` with `transport_setup`, `before_transport_configure`, `execute`. Anonymous-namespace helpers in the `.cpp` that Task 3 reuses: `struct Session`, `cancelled_if_requested`, `send`, `receive`, `has_sid`, `exchange_expect`, `expect_ssm_init`, `ecu_id`, and the constants `kTimeout`, `kExtraLongTimeout`, `kPage`, `kColdBaud`, `kEcuIdOffset`, `kEcuIdLength`. Task 3 adds `is_exact_reply` and `kMediumTimeout` with their first users, so nothing sits unused between tasks.
- Test helpers in the `_test.cpp` that Task 3 reuses: `request`, `reply`, `init_reply`, `kEcuId`, `kProtocol`, `kMcu`, `kRomSize`, `run`.

- [ ] **Step 1: Write the failing executor tests**

`src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h`:

```cpp
#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruUnisiaJecsM32rKlineExecutor final : public IKlineFlashExecutor
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

`src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
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

constexpr std::string_view kProtocol = "sub_ecu_unisia_jecs_20";
constexpr std::string_view kMcu = "M32R_128KB";
constexpr std::uint32_t kRomSize = 0x20000;
constexpr std::string_view kEcuId = "123456789A";

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

// FF, three capability bytes, the five ECU ID bytes at frame offset 8, then
// trailing capability bytes legacy discarded (read_mem() :162-163).
bytes::Bytes init_reply()
{
    return reply({0xff, 0xa1, 0xa2, 0xa3, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xb1, 0xb2});
}

bytes::Bytes page_data(std::uint32_t page)
{
    return bytes::Bytes(0x80, static_cast<bytes::Byte>(page * 7 + 1));
}

bytes::Bytes page_request(std::uint32_t address)
{
    return request({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16), static_cast<bytes::Byte>(address >> 8),
                    static_cast<bytes::Byte>(address), 0x7f});
}

bytes::Bytes page_reply(std::uint32_t page)
{
    bytes::Bytes payload{0xe0};
    const bytes::Bytes data = page_data(page);
    payload.insert(payload.end(), data.begin(), data.end());
    return reply(payload);
}

void script_warm_entry(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("read-mode probe answered");
    transport.exchange(request({0xbf}), init_reply());
}

void script_cold_entry(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("cold init");
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xb8, 0x00, 0x00, 0x00, 0x75}), reply({0xf8, 0x75}));
    transport.exchange(request({0xbf}), init_reply());
}

void script_pages(ScriptedKlineFlashTransport& transport, std::uint32_t count)
{
    auto section = transport.section("A0 pages");
    for (std::uint32_t page = 0; page < count; ++page)
    {
        transport.exchange(page_request(0x100000 + page * 0x80), page_reply(page));
    }
}

FlashPlan read_plan(std::string_view protocol = kProtocol, std::string_view mcu = kMcu)
{
    auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, protocol, mcu, std::nullopt, false);
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

struct Run
{
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
};

Result<FlashExecutionResult> run(const FlashPlan& plan, ScriptedKlineFlashTransport& transport, Run& context)
{
    return SubaruUnisiaJecsM32rKlineExecutor{}.execute(plan, transport, context.clock, context.cancellation,
                                                      context.events);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TransportSetupIs4800BaudPlainSsm)
{
    const auto setup = SubaruUnisiaJecsM32rKlineExecutor{}.transport_setup(read_plan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 4800);        // execute() :57
    EXPECT_FALSE(setup->iso14230);       // execute() :51
    EXPECT_EQ(setup->tester_id, 0xf0);   // execute() :55
    EXPECT_EQ(setup->target_id, 0x10);   // execute() :56
    EXPECT_EQ(setup->parity, KlineParity::None);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BeforeConfigureClearsTheIso14230Header)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruUnisiaJecsM32rKlineExecutor{}.before_transport_configure(transport, clock, cancellation), IsOk());
    EXPECT_EQ(transport.header_mode_calls_, std::vector<bool>{false});
    EXPECT_TRUE(transport.lifecycle_calls_.empty());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ReadsTheRomWhenAlreadyInReadMode)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    script_pages(transport, kRomSize / 0x80);
    Run context;

    auto result = run(read_plan(), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    ASSERT_EQ(result->read_bytes->size(), kRomSize);
    for (std::uint32_t page = 0; page < kRomSize / 0x80; ++page)
    {
        ASSERT_TRUE(std::equal(result->read_bytes->begin() + page * 0x80,
                               result->read_bytes->begin() + (page + 1) * 0x80, page_data(page).begin()))
            << page;
    }
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, std::vector<int>{38400});
    EXPECT_TRUE(transport.control_line_trace_.empty()) << "legacy read touched no LEC line";
    EXPECT_EQ(transport.read_timeouts_.front(), 2000ms);
    EXPECT_EQ(transport.read_timeouts_.back(), 3000ms);
    EXPECT_EQ(context.events.progress_calls.back(), (std::pair<int, int>{1024, 1024}));
    EXPECT_EQ(context.clock.elapsed(), 1024 * 1ms); // read_mem() :314
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitSwitchesBaudBeforeReading)
{
    ScriptedKlineFlashTransport transport;
    script_cold_entry(transport);
    script_pages(transport, kRomSize / 0x80);
    Run context;

    auto result = run(read_plan(), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{38400, 4800, 38400}));
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TruncatedProbeReplyFallsBackToColdInit)
{
    ScriptedKlineFlashTransport transport;
    // A valid FF frame too short to carry the five ID bytes.
    transport.exchange(request({0xbf}), reply({0xff, 0xa1}));
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xb8, 0x00, 0x00, 0x00, 0x75}), reply({0xf8, 0x75}));
    transport.exchange(request({0xbf}), init_reply());
    script_pages(transport, kRomSize / 0x80);
    Run context;

    auto result = run(read_plan(), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitRejectsABadBaudChangeReply)
{
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xb8, 0x00, 0x00, 0x00, 0x75}), reply({0x7f, 0xb8, 0x22}));
    Run context;

    auto result = run(read_plan(), transport, context);

    EXPECT_THAT(result, IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 3U);
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{38400, 4800}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitWithoutAnyReplyTimesOut)
{
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    transport.expectWrite(request({0xbf}));
    transport.queue_no_frame();
    Run context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ShortPageFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    bytes::Bytes short_page{0xe0};
    short_page.resize(0x80, 0x11); // SID + 127 data bytes
    transport.exchange(page_request(0x100000), reply(short_page));
    Run context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BadChecksumPageFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    bytes::Bytes corrupted = page_reply(0);
    corrupted.back() ^= 0x01;
    transport.exchange(page_request(0x100000), corrupted);
    Run context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, NegativePageReplyFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    transport.exchange(page_request(0x100000), reply({0x7f, 0xa0, 0x31}));
    Run context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, MissingPageTimesOut)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    transport.expectWrite(page_request(0x100000));
    transport.queue_no_frame();
    Run context;

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationStopsTheReadBeforeTheNextPage)
{
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    script_pages(transport, 2);
    Run context;
    // Trips once the probe and the first page request have been written.
    context.cancellation.set_predicate([&transport] { return transport.writesConsumed() >= 2; });

    EXPECT_THAT(run(read_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

class SubaruUnisiaJecsM32rKlineReadSizes
    : public ::testing::TestWithParam<std::tuple<std::string_view, std::string_view, std::uint32_t>>
{
};

TEST_P(SubaruUnisiaJecsM32rKlineReadSizes, ReadsEveryPageOfTheVariant)
{
    const auto& [protocol, mcu, rom_size] = GetParam();
    ScriptedKlineFlashTransport transport;
    script_warm_entry(transport);
    script_pages(transport, rom_size / 0x80);
    Run context;

    auto result = run(read_plan(protocol, mcu), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->read_bytes->size(), rom_size);
    EXPECT_TRUE(transport.scriptConsumed());
}

INSTANTIATE_TEST_SUITE_P(AllVariants, SubaruUnisiaJecsM32rKlineReadSizes,
                         ::testing::Values(std::tuple{"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000U},
                                           std::tuple{"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000U},
                                           std::tuple{"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000U},
                                           std::tuple{"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000U}));
} // namespace
} // namespace fastecu::flash
```

`std::tuple{"...", "...", 0x20000U}` deduces `const char *`; if the compiler rejects the conversion to `std::string_view`, spell each value `std::tuple<std::string_view, std::string_view, std::uint32_t>{...}`.

Add to `src/backend/flash/ecu/BUILD.bazel`, after the Task 1 targets:

```starlark
cc_library(
    name = "subaru_unisia_jecs_m32r_kline_executor",
    srcs = ["subaru_unisia_jecs_m32r_kline_executor.cpp"],
    hdrs = ["subaru_unisia_jecs_m32r_kline_executor.h"],
    deps = [
        ":subaru_unisia_jecs_m32r_kline_plan",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/ssm",
        "//src/backend/flash:flash_executor",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_unisia_jecs_m32r_kline_executor_test",
    srcs = ["subaru_unisia_jecs_m32r_kline_executor_test.cpp"],
    deps = [
        ":subaru_unisia_jecs_m32r_kline_executor",
        ":subaru_unisia_jecs_m32r_kline_plan",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:recording_event_sink",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

In `bazel/portable_targets.bzl`, after `"subaru_unisia_jecs_m32r_kline_plan",` add `"subaru_unisia_jecs_m32r_kline_executor",`.

Create `subaru_unisia_jecs_m32r_kline_executor.cpp` containing only `#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h"`.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_executor_test`
Expected: FAIL to link (undefined executor methods).

- [ ] **Step 3: Implement setup, helpers and Read**

Replace `subaru_unisia_jecs_m32r_kline_executor.cpp` with the code below. `write_rom` is a stub that Task 3 replaces; it keeps Write plans failing loudly until then.

```cpp
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;
using namespace std::chrono_literals;

// Legacy header timeouts (flash_ecu_subaru_unisia_jecs_m32r_operation.h).
constexpr auto kTimeout = 2000ms;          // serial_read_timeout
constexpr auto kExtraLongTimeout = 3000ms; // serial_read_extra_long_timeout
constexpr auto kPagePacing = 1ms;          // read_mem() :314
constexpr std::uint32_t kPage = 0x80;      // read_mem() :220, write_mem() :523
constexpr int kColdBaud = 4800;            // read_mem() :141, write_mem() :375
constexpr int kReadBaud = 38400;           // read_mem() :102, :195
// The ECU ID is the five bytes after the header, SID and three capability
// bytes of the BF reply (read_mem() :162-163).
constexpr std::size_t kEcuIdOffset = 8;
constexpr std::size_t kEcuIdLength = 5;

struct Session
{
    IKlineFlashTransport& transport;
    IClock& clock;
    const ICancellationToken& cancellation;
    IEventSink& events;
    const SubaruUnisiaJecsM32rKlinePlan& wire;
};

Status cancelled_if_requested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::format("cancelled {}", where));
    }
    return {};
}

// Legacy write_serial_data_echo_check() of an SsmProtocol::addHeader frame.
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

// One framed read; an empty optional means no frame arrived in time.
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

bool has_sid(bytes::ByteView frame, const SubaruUnisiaJecsM32rKlinePlan& wire, bytes::Byte sid)
{
    return SsmProtocol::hasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] >= 1 && frame[4] == sid;
}

bool carries_ecu_id(bytes::ByteView frame)
{
    return frame.size() >= kEcuIdOffset + kEcuIdLength + 1;
}

// Sends `payload` and requires a valid frame whose first payload byte is `sid`.
Result<bytes::Bytes> exchange_expect(Session& s, bytes::ByteView payload, std::chrono::milliseconds timeout,
                                     bytes::Byte sid, std::string_view what)
{
    if (Status sent = send(s, payload); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto response = receive(s, timeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return fail(ErrorKind::Timeout, std::format("no response to {}", what));
    }
    if (!has_sid(**response, s.wire, sid))
    {
        return fail(ErrorKind::BadResponse,
                    std::format("unexpected response to {}: {}", what, bytes::toHex(**response)));
    }
    return std::move(**response);
}

// send_sid_bf_ssm_init() :637-650, gated: the reply must be FF and long
// enough to carry the ECU ID.
Result<bytes::Bytes> expect_ssm_init(Session& s)
{
    auto init = exchange_expect(s, composeBe(0xbf_b), kTimeout, 0xff, "SSM init");
    if (!init.has_value())
    {
        return init;
    }
    if (!carries_ecu_id(*init))
    {
        return fail(ErrorKind::BadResponse, std::format("SSM init reply carries no ECU ID: {}", bytes::toHex(*init)));
    }
    return init;
}

std::string ecu_id(Session& s, bytes::ByteView init)
{
    std::string id;
    for (const bytes::Byte value : init.subspan(kEcuIdOffset, kEcuIdLength))
    {
        id += std::format("{:02X}", value);
    }
    s.events.log(LogLevel::Info, std::format("ECU ID: {}", id));
    return id;
}

Result<FlashExecutionResult> read_rom(Session& s, const FlashPlan& plan)
{
    // read_mem() :101-104: probe at 38400 in case the ECU is already in read
    // mode. A mismatch is logged and the cold init follows, as legacy did.
    if (Status baud = s.transport.setBaud(kReadBaud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    s.events.log(LogLevel::Info, "Checking if ECU in read mode");
    if (Status sent = send(s, composeBe(0xbf_b)); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto probe = receive(s, kTimeout);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    std::optional<bytes::Bytes> init;
    if (probe->has_value() && has_sid(**probe, s.wire, 0xff) && carries_ecu_id(**probe))
    {
        init = std::move(**probe);
    }
    else
    {
        s.events.log(LogLevel::Info, "Read mode not active, initialising ECU...");
        // read_mem() :141-216.
        if (Status baud = s.transport.setBaud(kColdBaud); !baud.has_value())
        {
            return std::unexpected(baud.error());
        }
        auto cold = expect_ssm_init(s);
        if (!cold.has_value())
        {
            return std::unexpected(cold.error());
        }
        init = std::move(*cold);
        // send_sid_b8_change_baudrate_38400() :671-688.
        auto changed =
            exchange_expect(s, composeBe(0xb8_b, 0x00_b, 0x00_b, 0x00_b, 0x75_b), kTimeout, 0xf8, "baud rate change");
        if (!changed.has_value())
        {
            return std::unexpected(changed.error());
        }
        if (Status baud = s.transport.setBaud(kReadBaud); !baud.has_value())
        {
            return std::unexpected(baud.error());
        }
        s.events.log(LogLevel::Info, "Requesting ECU ID, checking if baudrate change was ok");
        auto confirmed = expect_ssm_init(s);
        if (!confirmed.has_value())
        {
            return std::unexpected(confirmed.error());
        }
    }
    const std::string id = ecu_id(s, *init);

    // read_mem() :219-318. Every page must be a complete, checksummed E0
    // frame carrying exactly one page; legacy appended any E0 reply.
    const MemoryRegion region = plan.transfer_region();
    const auto pages = static_cast<int>(region.length / kPage);
    bytes::Bytes rom;
    rom.reserve(region.length);
    for (int page = 0; page < pages; ++page)
    {
        const std::uint32_t address = region.start + static_cast<std::uint32_t>(page) * kPage;
        auto block = exchange_expect(s, composeBe(0xa0_b, 0x00_b, u24(address), bytes::Byte(kPage - 1)),
                                     kExtraLongTimeout, 0xe0, std::format("block read at 0x{:06X}", address));
        if (!block.has_value())
        {
            return std::unexpected(block.error());
        }
        if (block->size() != 4 + 1 + kPage + 1)
        {
            return fail(ErrorKind::BadResponse, std::format("block read at 0x{:06X} returned {} bytes", address,
                                                            block->size()));
        }
        rom.insert(rom.end(), block->begin() + 5, block->begin() + 5 + kPage);
        s.events.progress(page + 1, pages);
        if (Status paced = s.clock.sleep(kPagePacing, s.cancellation); !paced.has_value())
        {
            return std::unexpected(paced.error());
        }
    }
    return FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = std::move(rom), .rom_id = id + "_"};
}

Status write_rom(Session&, const FlashPlan&)
{
    return fail(ErrorKind::Unsupported, "Unisia Jecs M32R write is not implemented yet");
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rKlineExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // execute() :50-57.
    return non_iso14230_kline_config_from(std::get<SubaruUnisiaJecsM32rKlinePlan>(plan.family_plan()));
}

Status SubaruUnisiaJecsM32rKlineExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                     const ICancellationToken&) const
{
    // execute() :50. Clears a header left enabled by an earlier session.
    return transport.set_add_iso14230_header(false);
}

Result<FlashExecutionResult> SubaruUnisiaJecsM32rKlineExecutor::execute(const FlashPlan& plan,
                                                                       IKlineFlashTransport& transport, IClock& clock,
                                                                       const ICancellationToken& cancellation,
                                                                       IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    Session session{transport, clock, cancellation, events,
                    std::get<SubaruUnisiaJecsM32rKlinePlan>(plan.family_plan())};
    if (plan.operation() == FlashOperation::Read)
    {
        return read_rom(session, plan);
    }

    const Status written = write_rom(session, plan);
    // execute() :71-72: the LEC lines drop after write_mem() on every outcome.
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

If `composeBe(0xbf_b)` with a single argument does not compile, use `bytes::Bytes{0xbf}`.

- [ ] **Step 4: Run to verify they pass**

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_executor_test
bazel build --config=release //:portable_closure
```
Expected: PASS.

- [ ] **Step 5: Mutation-check the read corrections**

For each mutation: apply it, run the test, confirm FAIL, then restore and confirm `git diff --exit-code src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.cpp`.

| Mutation in `read_rom` | Test that must fail |
|---|---|
| Delete the `block->size() != 4 + 1 + kPage + 1` check | `ShortPageFailsBeforeTheNextRequest` |
| In `has_sid`, replace `SsmProtocol::hasValidFrame(...)` with `frame.size() > 4` | `BadChecksumPageFailsBeforeTheNextRequest` |
| Drop `&& carries_ecu_id(**probe)` from the probe condition | `TruncatedProbeReplyFallsBackToColdInit` |
| Delete the `s.clock.sleep(kPagePacing, ...)` call | `ReadsTheRomWhenAlreadyInReadMode` |

- [ ] **Step 6: Commit**

```bash
git add bazel/portable_targets.bzl src/backend/flash/ecu
git commit -m "feat(flash): read Unisia Jecs M32R ROMs through a portable executor"
```

---

### Task 3: Executor — Write

**Files:**
- Modify: `src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.cpp`, `subaru_unisia_jecs_m32r_kline_executor_test.cpp`

**Interfaces:**
- Consumes: the Task 2 helpers named above; `IKlineFlashTransport::enable_programming_voltage_line`, `disable_lec_lines`; `ScriptedKlineFlashTransport::disable_lec_lines_result_`, `programming_voltage_line_write_index_`.
- Produces: the real `write_rom(Session&, const FlashPlan&) -> Status`, plus `is_exact_reply`, `poll_for`, `enter_flash_mode` and the write constants. No new public surface.

- [ ] **Step 1: Write the failing Write tests**

Append inside the anonymous namespace of `subaru_unisia_jecs_m32r_kline_executor_test.cpp`, before its closing `} // namespace`:

```cpp
const bytes::Bytes& rom_image()
{
    static const bytes::Bytes image = []
    {
        bytes::Bytes out(kRomSize);
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            out[i] = static_cast<bytes::Byte>(i * 3 + 1);
        }
        return out;
    }();
    return image;
}

FlashPlan write_plan()
{
    auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Write, kProtocol, kMcu, rom_image(), false);
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

bytes::Bytes block_request(std::uint32_t index)
{
    const std::uint32_t address = index * 0x80;
    const bool last = index == kRomSize / 0x80 - 1;
    bytes::Bytes payload{0xaf, static_cast<bytes::Byte>(last ? 0x69 : 0x61), static_cast<bytes::Byte>(address >> 16),
                         static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address)};
    for (std::uint32_t j = 0; j < 0x80; ++j)
    {
        payload.push_back(static_cast<bytes::Byte>(rom_image()[address + j] ^ 0x82));
    }
    return request(payload);
}

const bytes::Bytes kEraseStarted = reply({0xef, 0x42});
const bytes::Bytes kDone = reply({0xef, 0x52});

void script_obk_running(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("OBK probe answered");
    transport.exchange(request({0xaf}), reply({0xef}));
}

void script_cold_flash_mode(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("enter flash mode");
    transport.expectWrite(request({0xaf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xaf, 0x11, 0x12, 0x34, 0x56, 0x78, 0x9a, 0x02, 0x00, 0x00}), reply({0xef}));
}

// One empty poll before each erase reply, then the trailing read.
void script_erase(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("erase");
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queue_no_frame();
    transport.queueRead(kEraseStarted);
    transport.queue_no_frame();
    transport.queueRead(kDone);
    transport.queue_no_frame();
}

void script_blocks(ScriptedKlineFlashTransport& transport, std::uint32_t count)
{
    auto section = transport.section("program blocks");
    for (std::uint32_t index = 0; index < count; ++index)
    {
        transport.exchange(block_request(index), kDone);
    }
}

std::uint32_t block_count()
{
    return kRomSize / 0x80;
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, WritesWhenTheKernelIsAlreadyRunning)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, block_count());
    Run context;

    auto result = run(write_plan(), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_FALSE(result->rom_id.has_value()) << "legacy set RomId only on read";
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, std::vector<int>{19200});
    EXPECT_EQ(transport.control_line_trace_, (std::vector<Line>{Line::EnableProgrammingVoltageLine,
                                                                Line::DisableLecLines}));
    // After the OBK probe, before AF 31 (write_mem() :441-444).
    EXPECT_EQ(transport.programming_voltage_line_write_index_, std::optional<std::size_t>(1));
    // One 500 ms sleep after each empty erase poll.
    EXPECT_EQ(context.clock.elapsed(), 1000ms);
    EXPECT_EQ(context.events.progress_calls.back(), (std::pair<int, int>{1024, 1024}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EntersFlashModeFromCold)
{
    ScriptedKlineFlashTransport transport;
    script_cold_flash_mode(transport);
    script_erase(transport);
    script_blocks(transport, block_count());
    Run context;

    ASSERT_THAT(run(write_plan(), transport, context), IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{19200, 4800, 19200}));
    EXPECT_EQ(transport.programming_voltage_line_write_index_, std::optional<std::size_t>(3));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, RejectedFlashModeEntryNeverRaisesProgrammingVoltage)
{
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf}));
    transport.queue_no_frame();
    transport.exchange(request({0xbf}), init_reply());
    transport.exchange(request({0xaf, 0x11, 0x12, 0x34, 0x56, 0x78, 0x9a, 0x02, 0x00, 0x00}),
                       reply({0x7f, 0xaf, 0x22}));
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace_, std::vector<Line>{Line::DisableLecLines});
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EraseStartExhaustionFails)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.queue_no_frame();
    }
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(context.clock.elapsed(), 20 * 500ms);
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EraseCompleteExhaustionFailsInsteadOfProgramming)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(kEraseStarted);
    for (int round = 0; round < 40; ++round)
    {
        transport.queue_no_frame();
    }
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 2U) << "legacy went on to program blank flash";
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, UnexpectedEraseReplyFails)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x48}));
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BadBlockReplyStopsBeforeTheNextBlock)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    transport.exchange(block_request(0), reply({0xef, 0x5a}));
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, FinalBlockSilenceSucceedsWithAWarning)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, block_count() - 1);
    transport.expectWrite(block_request(block_count() - 1));
    transport.queue_no_frame();
    Run context;

    ASSERT_THAT(run(write_plan(), transport, context), IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_TRUE(std::ranges::any_of(context.events.logs,
                                    [](const auto& log) { return log.first == LogLevel::Warning; }));
    EXPECT_EQ(transport.read_timeouts_.back(), 3000ms);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, FinalBlockBadReplyFails)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, block_count() - 1);
    transport.exchange(block_request(block_count() - 1), reply({0x7f, 0xaf, 0x22}));
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationDuringProgrammingReportsCancelledAndDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, 2);
    Run context;
    // Trips as soon as the first block has been written.
    context.cancellation.set_predicate([&transport] { return transport.writesConsumed() >= 3; });

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationDuringTheErasePollDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.queue_no_frame();
    }
    Run context;
    // Trips on the third erase poll.
    context.cancellation.set_predicate([&transport] { return transport.read_timeouts_.size() >= 4; });

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 2U);
    EXPECT_EQ(transport.control_line_trace_, (std::vector<Line>{Line::EnableProgrammingVoltageLine,
                                                                Line::DisableLecLines}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TransportErrorMidProgrammingPropagatesAndDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    transport.expectWrite(block_request(0));
    transport.queue_error(ErrorKind::Disconnected, "adapter unplugged");
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CleanupFailureFailsAnOtherwiseSuccessfulWrite)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    script_erase(transport);
    script_blocks(transport, block_count());
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "RTS stuck");
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::Disconnected));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CleanupFailureNeverReplacesAnEarlierError)
{
    ScriptedKlineFlashTransport transport;
    script_obk_running(transport);
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x48}));
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "RTS stuck");
    Run context;

    EXPECT_THAT(run(write_plan(), transport, context), IsErr(ErrorKind::BadResponse));
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_executor_test`
Expected: the Task 2 tests PASS; every new Write test FAILS (the stub returns `Unsupported`), except `RejectedFlashModeEntryNeverRaisesProgrammingVoltage` and the two cleanup tests, which may fail for the same reason.

- [ ] **Step 3: Implement the write**

In `subaru_unisia_jecs_m32r_kline_executor.cpp`, add to the constants block:

```cpp
constexpr auto kMediumTimeout = 500ms;     // serial_read_medium_timeout
constexpr int kWriteBaud = 19200;          // write_mem() :347, :431
constexpr auto kErasePollSleep = 500ms;    // write_mem() :475, :515
constexpr int kEraseStartRounds = 20;      // write_mem() :448
constexpr int kEraseDoneRounds = 40;       // write_mem() :488
constexpr bytes::Byte kBlockXor = 0x82;    // write_mem() :525, :557
```

Then replace the `write_rom` stub with:

```cpp
bool is_exact_reply(bytes::ByteView frame, const SubaruUnisiaJecsM32rKlinePlan& wire, bytes::ByteView payload)
{
    return SsmProtocol::hasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] == payload.size() &&
           std::ranges::equal(frame.subspan(4, payload.size()), payload);
}

// write_mem() :448-516. read() returns whole frames, so legacy's byte
// accumulation has no counterpart: an empty read continues the poll, and any
// frame returned must be exactly `expected`. Exhausting the rounds fails;
// legacy's second poll fell through to programming.
Status poll_for(Session& s, bytes::ByteView expected, int rounds, std::string_view what)
{
    for (int round = 0; round < rounds; ++round)
    {
        auto response = receive(s, kMediumTimeout);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->has_value())
        {
            if (!is_exact_reply(**response, s.wire, expected))
            {
                return fail(ErrorKind::BadResponse, std::format("{} failed: {}", what, bytes::toHex(**response)));
            }
            return {};
        }
        if (Status slept = s.clock.sleep(kErasePollSleep, s.cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return fail(ErrorKind::Timeout, std::format("no {} response after {} polls", what, rounds));
}

// write_mem() :346-432. Returns once the ECU is in flash mode at 19200 baud.
Status enter_flash_mode(Session& s, std::uint32_t rom_size)
{
    if (Status baud = s.transport.setBaud(kWriteBaud); !baud.has_value())
    {
        return baud;
    }
    s.events.log(LogLevel::Info, "Checking if OBK is running");
    if (Status sent = send(s, composeBe(0xaf_b)); !sent.has_value())
    {
        return sent;
    }
    auto probe = receive(s, kTimeout);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (probe->has_value() && has_sid(**probe, s.wire, 0xef))
    {
        return {};
    }
    s.events.log(LogLevel::Info, "OBK not running, requesting flash mode");

    if (Status baud = s.transport.setBaud(kColdBaud); !baud.has_value())
    {
        return baud;
    }
    auto init = expect_ssm_init(s);
    if (!init.has_value())
    {
        return std::unexpected(init.error());
    }
    (void)ecu_id(s, *init);
    // send_sid_af_enter_flash_mode() :690-714. Gated: legacy logged a
    // rejection here and went on to raise VPP and erase.
    s.events.log(LogLevel::Info, "Sending request to change to flash mode");
    auto entered = exchange_expect(
        s, composeBe(0xaf_b, 0x11_b, bytes::ByteView(*init).subspan(kEcuIdOffset, kEcuIdLength), u24(rom_size)),
        kTimeout, 0xef, "enter flash mode");
    if (!entered.has_value())
    {
        return std::unexpected(entered.error());
    }
    s.events.log(LogLevel::Debug, "Changing baudrate to 19200");
    return s.transport.setBaud(kWriteBaud);
}

Status write_rom(Session& s, const FlashPlan& plan)
{
    const bytes::Bytes& image = *plan.image();
    const std::uint32_t rom_size = plan.transfer_region().length;
    if (Status entered = enter_flash_mode(s, rom_size); !entered.has_value())
    {
        return entered;
    }

    // write_mem() :440-441. The operator confirmed external VPP before the
    // run when the adapter cannot supply it.
    if (Status cancelled = cancelled_if_requested(s.cancellation, "before programming voltage");
        !cancelled.has_value())
    {
        return cancelled;
    }
    s.events.log(LogLevel::Debug, "Set programming voltage +12v to Line End Check 1");
    if (Status raised = s.transport.enable_programming_voltage_line(); !raised.has_value())
    {
        return raised;
    }

    // send_sid_af_erase_memory_block() :716-731 sends without reading.
    s.events.log(LogLevel::Info, "Sending request to erase flash");
    if (Status sent = send(s, composeBe(0xaf_b, 0x31_b)); !sent.has_value())
    {
        return sent;
    }
    if (Status started = poll_for(s, composeBe(0xef_b, 0x42_b), kEraseStartRounds, "flash erase start");
        !started.has_value())
    {
        return started;
    }
    s.events.log(LogLevel::Info, "Flash erase in progress, please wait...");
    if (Status erased = poll_for(s, composeBe(0xef_b, 0x52_b), kEraseDoneRounds, "flash erase");
        !erased.has_value())
    {
        return erased;
    }
    s.events.log(LogLevel::Info, "Flash erased!");
    // write_mem() :517: one more read, whose result legacy discarded.
    auto trailing = receive(s, kMediumTimeout);
    if (!trailing.has_value())
    {
        return std::unexpected(trailing.error());
    }
    if (trailing->has_value())
    {
        s.events.log(LogLevel::Debug, std::format("Discarded after erase: {}", bytes::toHex(**trailing)));
    }

    // write_mem() :535-625.
    const auto blocks = static_cast<int>(rom_size / kPage);
    const bytes::Bytes done = composeBe(0xef_b, 0x52_b);
    for (int block = 0; block < blocks; ++block)
    {
        const std::uint32_t address = static_cast<std::uint32_t>(block) * kPage;
        const bool last = block == blocks - 1;
        bytes::Bytes data(image.begin() + address, image.begin() + address + kPage);
        for (bytes::Byte& value : data)
        {
            value ^= kBlockXor;
        }
        if (Status sent = send(s, composeBe(0xaf_b, last ? 0x69_b : 0x61_b, u24(address), bytes::ByteView(data)));
            !sent.has_value())
        {
            return sent;
        }
        auto response = receive(s, kExtraLongTimeout);
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
            // Legacy never read a reply to AF 69 (:564); its shape is unknown.
            s.events.log(LogLevel::Warning, "No reply to the final block; treating the write as complete");
        }
        else if (!is_exact_reply(**response, s.wire, done))
        {
            return fail(ErrorKind::BadResponse,
                        std::format("block write at 0x{:06X} failed: {}", address, bytes::toHex(**response)));
        }
        s.events.progress(block + 1, blocks);
    }
    s.events.log(LogLevel::Info, "ROM written to flash.");
    return {};
}
```

- [ ] **Step 4: Run to verify they pass**

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_executor_test
```
Expected: PASS.

- [ ] **Step 5: Mutation-check the gating**

Apply each mutation, run the test, confirm FAIL, restore, and confirm `git diff --exit-code src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.cpp`.

| Mutation | Test that must fail |
|---|---|
| In `enter_flash_mode`, ignore `entered`'s error (replace the `if (!entered.has_value()) return ...` with nothing) | `RejectedFlashModeEntryNeverRaisesProgrammingVoltage` |
| In `poll_for`, replace the final `return fail(ErrorKind::Timeout, ...)` with `return {};` | `EraseCompleteExhaustionFailsInsteadOfProgramming` |
| In the block loop, change `else if (!is_exact_reply(**response, s.wire, done))` to `else if (!last && !is_exact_reply(**response, s.wire, done))` | `FinalBlockBadReplyFails` |
| In `execute`, move `transport.disable_lec_lines()` into the success path only | `CancellationDuringProgrammingReportsCancelledAndDropsTheLine` |
| In `execute`, check `dropped` before `written` | `CleanupFailureNeverReplacesAnEarlierError` |
| Change `kBlockXor` to `0x00` | `WritesWhenTheKernelIsAlreadyRunning` |

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu
git commit -m "feat(flash): write Unisia Jecs M32R ROMs with gated erase and programming"
```

---

### Task 4: Desktop workflow, prompts, dispatch cut-over, legacy deletion

**Files:**
- Modify: `src/platform/desktop/common/transport/desktop_kline_flash_transport.{h,cpp}`, `desktop_kline_flash_transport_test.cpp`
- Modify: `src/platform/desktop/common/flash/flash_workflow.h:35-44`, `flash_workflow.cpp`, `flash_workflow_test.cpp`, `src/platform/desktop/common/flash/BUILD.bazel:73-74`
- Modify: `src/ui/desktop/flash/common/flash_dialog.cpp:194`, `src/ui/desktop/mainwindow.h:65`, `src/ui/desktop/mainwindow.cpp:1256-1279`, `src/ui/desktop/BUILD.bazel:109`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`, `src/platform/desktop/common/serial/BUILD.bazel:82`, `scripts/check-legacy-flash-drain.py:35`, `scripts/check-serial-compat-allowlist.py:32`
- Delete: `src/ui/desktop/flash/ecu/`, `src/platform/desktop/common/flash/legacy/ecu/`

**Interfaces:**
- Consumes: `build_subaru_unisia_jecs_m32r_kline_plan` (Task 1), `SubaruUnisiaJecsM32rKlineExecutor` (Tasks 2–3), `ConfirmationSpec::Id::ApplyProgrammingVoltage` (Task 1); `DesktopKlineFlashTransport`, `QtClock`, `bind_flash_attempt`, `FlashAttemptOutcome` (existing, in `flash_workflow.cpp`).
- Produces: `bool adapter_supplies_programming_voltage(SerialPortActions *serial);` in `namespace fastecu::flash`, declared in `desktop_kline_flash_transport.h`; `FlashPromptKind::ApplyProgrammingVoltage`, `FlashPromptKind::RemoveProgrammingVoltage` (the latter carries argument `{"outcome", "succeeded" | "failed" | "cancelled"}`), `SubaruUnisiaJecsM32rKlineWorkflow`, route kind `SubaruUnisiaJecsM32rKline`.
- Why the adapter query lives in the transport package: `flash_workflow.h` only forward-declares `SerialPortActions`, and the flash package is not on the frozen `serial_qt_compat` visibility list, so it cannot include `serial_port_actions.h`. `//src/platform/desktop/common/transport:flash_transports` already may, and `:flash_worker` already depends on it. This is a desktop helper, not a port method.

- [ ] **Step 0: Add the adapter query, test-first**

In `src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp`, add this slot after `postKernelUploadDelayCapabilityMirrorsOpenPort2OnUnix()`:

```cpp
    void programmingVoltageSupplyMirrorsOpenPort2OnEveryPlatform()
    {
        FakeBackedSerial serial;
        SerialPortActions *serial_ptr = serial.get();
        QVERIFY(!fastecu::flash::adapter_supplies_programming_voltage(nullptr));
        QVERIFY(!fastecu::flash::adapter_supplies_programming_voltage(serial_ptr));
        QVERIFY(serial_ptr->set_use_openport2_adapter(true));
        QVERIFY(fastecu::flash::adapter_supplies_programming_voltage(serial_ptr));
    }
```

Run `bazel test --config=release //src/platform/desktop/common/transport:test_desktop_kline_flash_transport` and confirm it fails to compile.

In `desktop_kline_flash_transport.h`, after the `DesktopKlineFlashTransport` class, inside the namespace, add:

```cpp
// Whether the adapter drives programming voltage on the LEC line itself
// (OpenPort 2.0), so the operator need not apply it. Unlike
// requires_post_kernel_upload_delay(), this holds on every platform. A null
// serial answers false: without adapter information the caller prompts.
bool adapter_supplies_programming_voltage(SerialPortActions *serial);
```

In `desktop_kline_flash_transport.cpp`, inside the namespace, add:

```cpp
bool adapter_supplies_programming_voltage(SerialPortActions *serial)
{
    return serial != nullptr && serial->get_use_openport2_adapter();
}
```

Run the test again and confirm it passes.


- [ ] **Step 1: Write the failing workflow tests**

In `src/platform/desktop/common/flash/flash_workflow_test.cpp`:

- In `recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone`, append to `portable` after `"sub_ecu_denso_mc68hc16y5_02_bdm"`:
  ```cpp
                                                                    "sub_ecu_unisia_jecs_20",
                                                                    "sub_ecu_unisia_jecs_30",
                                                                    "sub_ecu_unisia_jecs_40",
                                                                    "sub_ecu_unisia_jecs_70",
  ```
- Add these declarations to the `private slots:` list after `densoSh705xKlineIgnoresPrefixLookalikes();`:
  ```cpp
      void unisiaJecsM32rRoutesTheFourExactProtocols();
      void unisiaJecsM32rBootmodeAndLookalikesStayLegacy();
      void unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter();
      void unisiaJecsM32rFailedWriteRemindsBeforeReportingTheFailure();
      void unisiaJecsM32rCancelledWriteReminds();
      void unisiaJecsM32rDeclinedVppPromptCancelsBeforeAttempt();
      void unisiaJecsM32rAdapterSuppliedVppSkipsBothPrompts();
      void unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts();
      void unisiaJecsM32rWriteOnReadOnlyVariantFailsBeforeAnyPrompt();
  ```
- Add, inside the anonymous namespace after `request()`:
  ```cpp
  FlashWorkflowRequest unisiaM32rWrite()
  {
      auto input = request("sub_ecu_unisia_jecs_20", FlashOperation::Write);
      input.mcu = "M32R_128KB";
      input.image = bytes::Bytes(0x20000, 0xff);
      return input;
  }

  // Begin -> ApplyProgrammingVoltage -> attempt, all accepted.
  std::unique_ptr<FlashWorkflow> unisiaM32rWriteAtAttempt()
  {
      auto workflow = FlashWorkflowFactory::tryCreate(unisiaM32rWrite());
      if (workflow == nullptr || std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::Begin)
      {
          return nullptr;
      }
      workflow->submit(FlashPromptResponse::Accept);
      if (std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::ApplyProgrammingVoltage)
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

  using PromptArguments = std::vector<std::pair<std::string, std::string>>;
  ```
- Add the test bodies before `QTEST_MAIN`:

```cpp
void FlashWorkflowTest::unisiaJecsM32rRoutesTheFourExactProtocols()
{
    struct Variant
    {
        const char *protocol;
        const char *mcu;
        std::uint32_t rom_size;
    };
    for (const Variant& variant : std::to_array<Variant>({
             {"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000},
             {"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000},
             {"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000},
             {"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000},
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
    }
}

void FlashWorkflowTest::unisiaJecsM32rBootmodeAndLookalikesStayLegacy()
{
    for (const char *protocol : {"sub_ecu_unisia_jecs_20_bootmode", "sub_ecu_unisia_jecs_30_bootmode",
                                 "sub_ecu_unisia_jecs_20x", "sub_ecu_unisia_jecs_7"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) == nullptr, protocol);
    }
}

void FlashWorkflowTest::unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter()
{
    // request() carries a null serial: no adapter information means prompting.
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaM32rWrite());
    QVERIFY(workflow != nullptr);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ApplyProgrammingVoltage);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    QCOMPARE(plan.confirmations().size(), std::size_t{1});
    QCOMPARE(plan.confirmations()[0].id, ConfirmationSpec::Id::ApplyProgrammingVoltage);

    workflow->submit(FlashAttemptResult{.success = true});
    const auto reminder = std::get<FlashPromptStep>(workflow->next());
    QCOMPARE(reminder.kind, FlashPromptKind::RemoveProgrammingVoltage);
    QVERIFY(reminder.arguments == (PromptArguments{{"outcome", "succeeded"}}));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
}

void FlashWorkflowTest::unisiaJecsM32rFailedWriteRemindsBeforeReportingTheFailure()
{
    auto workflow = unisiaM32rWriteAtAttempt();
    QVERIFY(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::BadResponse, .error_detail = "x"});
    const auto reminder = std::get<FlashPromptStep>(workflow->next());
    QCOMPARE(reminder.kind, FlashPromptKind::RemoveProgrammingVoltage);
    QVERIFY(reminder.arguments == (PromptArguments{{"outcome", "failed"}}));
    workflow->submit(FlashPromptResponse::Accept);
    const auto failure = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(failure));
    QCOMPARE(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::BadResponse);
}

void FlashWorkflowTest::unisiaJecsM32rCancelledWriteReminds()
{
    auto workflow = unisiaM32rWriteAtAttempt();
    QVERIFY(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::Cancelled});
    const auto reminder = std::get<FlashPromptStep>(workflow->next());
    QCOMPARE(reminder.kind, FlashPromptKind::RemoveProgrammingVoltage);
    QVERIFY(reminder.arguments == (PromptArguments{{"outcome", "cancelled"}}));
    workflow->submit(FlashPromptResponse::Decline); // OK-only notice; any answer continues
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::unisiaJecsM32rDeclinedVppPromptCancelsBeforeAttempt()
{
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaM32rWrite());
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ApplyProgrammingVoltage);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::unisiaJecsM32rAdapterSuppliedVppSkipsBothPrompts()
{
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    QVERIFY(serial != nullptr);
    QVERIFY(serial->set_use_openport2_adapter(true));
    auto input = unisiaM32rWrite();
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    QVERIFY(std::get<FlashAttempt>(step).attempt->plan().confirmations().empty());
    workflow->submit(FlashAttemptResult{.success = true});
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
}

void FlashWorkflowTest::unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts()
{
    auto input = request("sub_ecu_unisia_jecs_30");
    input.mcu = "M32R_256KB";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{
        .success = true, .read_bytes = bytes::Bytes{1, 2}, .rom_id = std::string("123456789A_")});
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QVERIFY(std::get<FlashCompletedStep>(done).rom_id == std::optional<std::string>("123456789A_"));
    QVERIFY(std::get<FlashCompletedStep>(done).accepted_read_bytes == std::optional<bytes::Bytes>(bytes::Bytes{1, 2}));
}

void FlashWorkflowTest::unisiaJecsM32rWriteOnReadOnlyVariantFailsBeforeAnyPrompt()
{
    auto input = request("sub_ecu_unisia_jecs_40", FlashOperation::Write);
    input.mcu = "M32R_384KB";
    input.image = bytes::Bytes(0x60000, 0xff);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}
```

If `FlashAttemptResult`'s designated initializers reject skipping `error_kind`, spell every field in declaration order: `success`, `error_kind`, `error_detail`, `read_bytes`, `rom_id`.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow`
Expected: FAIL to compile (`FlashPromptKind::ApplyProgrammingVoltage` and `RemoveProgrammingVoltage` are undeclared).

- [ ] **Step 3: Implement the workflow**

In `src/platform/desktop/common/flash/flash_workflow.h`, after `ConfirmBdmKernelBootstrap,` add:
```cpp
    // Wave 6c-3. Before the attempt: the operator applies external VPP.
    ApplyProgrammingVoltage,
    // Wave 6c-3. After the attempt, OK-only: argument "outcome" is
    // "succeeded", "failed" or "cancelled".
    RemoveProgrammingVoltage,
```

In `src/platform/desktop/common/flash/flash_workflow.cpp`:
- add, after the `subaru_unisia_jecs_plan.h` include:
  ```cpp
  #include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h"
  #include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
  ```
- add, after `class SubaruUnisiaJecsWorkflow` (before `class SubaruDensoMc68hc16y5_02BdmWorkflow`):

```cpp
// Wave 6c-3. The adapter check moved here from legacy write_mem() :434: an
// adapter that supplies programming voltage needs no operator prompt. With no
// serial at all the workflow cannot know, so it prompts.
class SubaruUnisiaJecsM32rKlineWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruUnisiaJecsM32rKlineWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)),
          plan_(build_subaru_unisia_jecs_m32r_kline_plan(
              request_.operation, request_.protocol, request_.mcu, std::move(request_.image),
              adapter_supplies_programming_voltage(request_.serial)))
    {
        if (plan_.has_value())
        {
            needs_vpp_ = std::ranges::any_of(plan_->confirmations(), [](const ConfirmationSpec& spec) {
                return spec.id == ConfirmationSpec::Id::ApplyProgrammingVoltage;
            });
        }
    }

    FlashWorkflowStep next() override
    {
        if (!plan_.has_value())
        {
            return FlashFailureStep{plan_.error()};
        }
        // The reminder precedes whatever the attempt produced, failure included.
        if (stage_ == Stage::RemoveVpp)
        {
            return FlashPromptStep{FlashPromptKind::RemoveProgrammingVoltage, {{"outcome", attempt_outcome_}}};
        }
        if (outcome_.hasFailure())
        {
            return outcome_.takeFailure();
        }
        if (outcome_.terminal())
        {
            return outcome_.completedStep();
        }
        if (stage_ == Stage::Begin)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (stage_ == Stage::ApplyVpp)
        {
            return FlashPromptStep{FlashPromptKind::ApplyProgrammingVoltage, {}};
        }
        if (stage_ == Stage::Attempt)
        {
            stage_ = Stage::Done;
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     bind_flash_attempt(std::move(*plan_),
                                                        std::make_unique<SubaruUnisiaJecsM32rKlineExecutor>(),
                                                        std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                     std::make_unique<QtClock>()};
        }
        return outcome_.completedStep();
    }

    void submit(FlashPromptResponse response) override
    {
        switch (stage_)
        {
        case Stage::Begin:
        case Stage::ApplyVpp:
            if (response != FlashPromptResponse::Accept)
            {
                outcome_.cancel();
                return;
            }
            stage_ = stage_ == Stage::Begin && needs_vpp_ ? Stage::ApplyVpp : Stage::Attempt;
            return;
        case Stage::RemoveVpp:
            stage_ = Stage::Done; // OK-only notice
            return;
        case Stage::Attempt:
        case Stage::Done:
            return;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        if (needs_vpp_)
        {
            attempt_outcome_ = result.success                               ? "succeeded"
                               : result.error_kind == ErrorKind::Cancelled ? "cancelled"
                                                                           : "failed";
            stage_ = Stage::RemoveVpp;
        }
        outcome_.record(std::move(result));
    }

  private:
    enum class Stage
    {
        Begin,
        ApplyVpp,
        Attempt,
        RemoveVpp,
        Done,
    };

    FlashWorkflowRequest request_;
    Result<FlashPlan> plan_;
    bool needs_vpp_ = false;
    Stage stage_ = Stage::Begin;
    std::string attempt_outcome_;
    FlashAttemptOutcome outcome_;
};
```

- in `struct Route::Kind`, after `SubaruDensoMc68hc16y5_02Bdm,` add `SubaruUnisiaJecsM32rKline,`;
- in `kRoutes`, after the two `sub_ecu_unisia_jecs_m377x` rows add:
  ```cpp
      // Exact only: the _bootmode names share these prefixes and stay on the
      // legacy MainWindow path until wave 7.
      {"sub_ecu_unisia_jecs_20", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
      {"sub_ecu_unisia_jecs_30", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
      {"sub_ecu_unisia_jecs_40", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
      {"sub_ecu_unisia_jecs_70", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
  ```
- in `tryCreate`'s switch, after the `SubaruDensoMc68hc16y5_02Bdm` case add:
  ```cpp
      case SubaruUnisiaJecsM32rKline:
          return std::make_unique<SubaruUnisiaJecsM32rKlineWorkflow>(std::move(request));
  ```

In `src/platform/desktop/common/flash/BUILD.bazel`, after `"//src/backend/flash/ecu:subaru_unisia_jecs_plan",` add:
```starlark
        "//src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_executor",
        "//src/backend/flash/ecu:subaru_unisia_jecs_m32r_kline_plan",
```
In `src/ui/desktop/flash/common/flash_dialog.cpp`, before the `ConfirmSh7058Read` branch add:

```cpp
    if (prompt.kind == FlashPromptKind::ApplyProgrammingVoltage)
    {
        return QMessageBox::warning(this, tr("Programming voltage"),
                                    tr("Apply VPP voltage to the ECU, then press OK to continue."),
                                    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Ok
                   ? FlashPromptResponse::Accept
                   : FlashPromptResponse::Decline;
    }
    if (prompt.kind == FlashPromptKind::RemoveProgrammingVoltage)
    {
        QString text = tr("Remove VPP voltage from the ECU, then press OK.");
        if (arg("outcome") != QStringLiteral("succeeded"))
        {
            text += QStringLiteral("\n\n") +
                    tr("The write did not complete. If the ECU entered flash mode, do not power it off: the flash "
                       "kernel is still running and you can try flashing again.");
        }
        QMessageBox::information(this, tr("Programming voltage"), text);
        return FlashPromptResponse::Accept;
    }
```

- [ ] **Step 4: Run to verify they pass**

Run: `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow //src/ui/desktop/flash/common:all`
Expected: PASS.

- [ ] **Step 5: Cut over MainWindow and delete the legacy path**

- In `src/ui/desktop/mainwindow.cpp`, delete the block:
  ```cpp
          /*
           * Unisia Jecs ECU
           */
          else if (configValues->flash_protocol_selected_protocol_name.startsWith("sub_ecu_unisia_jecs_20"))
          {
              FlashEcuSubaruUnisiaJecsM32r flash_module(serial, ecuCalDef[rom_number], cmd_type, this);
              connect_signals_and_run_module(&flash_module);
          }
  ```
  and the three `else if` branches that follow it for `_30`, `_40` and `_70`. The `_30_bootmode` branch before it now continues directly into the `/* Hitachi ECU Boot Mode */` comments and the `else` for unknown flash methods.
- In `src/ui/desktop/mainwindow.h`, delete `#include "src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs_m32r.h"` and the blank line after it.
- In `src/ui/desktop/BUILD.bazel`, delete `"//src/ui/desktop/flash/ecu",`.
- In `src/platform/desktop/common/flash/legacy/BUILD.bazel`:
  - delete `"ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",` from `srcs` and `"ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.h",` from `hdrs`;
  - in the comment above `hdrs`, change `the bootmode/ecu legacy Qt operation classes` to `the bootmode legacy Qt operation class`, change `Every *_operation.h in the two family subdirectories declares Q_OBJECT.` to `The *_operation.h in the bootmode subdirectory declares Q_OBJECT.`, and append `The ecu/ subdirectory is gone: wave 6c-3 drained it.` after the jtag sentence;
  - change `Every *.h in the three directories (root + two families)` to `Every *.h in the two directories (root + bootmode)`.
  Read the comment before editing; if its wording differs, keep its meaning and update the counts.
- In `src/platform/desktop/common/serial/BUILD.bazel`, delete `"//src/ui/desktop/flash/ecu:__pkg__",` from the `serial_qt_compat` visibility list.
- In `scripts/check-serial-compat-allowlist.py`, delete `"//src/ui/desktop/flash/ecu:__pkg__",` from `FROZEN`.
- In `scripts/check-legacy-flash-drain.py`, delete `"ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",` from `REMAINING`.
- Delete the files:
  ```bash
  git rm -r src/ui/desktop/flash/ecu src/platform/desktop/common/flash/legacy/ecu
  ```
- Confirm nothing else references them: `grep -rn "FlashEcuSubaruUnisiaJecsM32r\b\|FlashEcuSubaruUnisiaJecsM32rOperation\|desktop/flash/ecu\|legacy/ecu" src apps tests bazel BUILD.bazel scripts`. Expect no matches. (`FlashEcuSubaruUnisiaJecsM32rBootMode` matches neither pattern and must stay.)

- [ ] **Step 6: Build and run the affected suites and guards**

```bash
bazel build --config=release //:fastecu //:portable_closure
bazel test --config=release //src/platform/desktop/common/flash/... //src/platform/desktop/common/transport/... //src/backend/flash/... //src/ui/desktop/... //:legacy_flash_drain //:serial_compat_allowlist --test_output=errors
```
Expected: all PASS. The drain test logs `OK: 1 families remaining, none added.`; the allowlist test logs `OK: 8 entries, none added.` with no "Update FROZEN" line.

- [ ] **Step 7: Commit**

```bash
git add -A src/platform/desktop/common src/ui/desktop scripts
git commit -m "feat(flash): route Unisia Jecs M32R through the portable workflow"
```

---

### Task 5: Docs, gates, PR

**Files:**
- Modify: `docs/flash-qualification-matrix.md:53`, `docs/modularization-plan.md:81-84, 219`, `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md` (status line + appended note), `docs/superpowers/specs/2026-09-25-step5-tail-wave6c3-unisia-jecs-m32r-kline-design.md` (status + checklist link)
- Create: `docs/unisia-jecs-m32r-bench-checklist.md`

- [ ] **Step 1: Flip the matrix row**

Replace the `FlashEcuSubaruUnisiaJecsM32r` row with (one line):

```markdown
| FlashEcuSubaruUnisiaJecsM32r | ECU | K-Line | read (all four); write (`_20`, `_30`) | yes | `subaru_unisia_jecs_m32r_kline_plan_test`, `subaru_unisia_jecs_m32r_kline_executor_test`, `test_flash_workflow` @ Wave 6c-3 | experimental | — | Exact protocol/MCU pairs `sub_ecu_unisia_jecs_20` / `M32R_128KB`, `_30` / `M32R_256KB`, `_40` / `M32R_384KB` and `_70` / `M32R_512KB`; `_40` and `_70` are read-only and test_write is rejected everywhere, matching the cfg. The `_bootmode` protocols stay on the legacy path. Framed SSM over plain K-Line: read at 38400 baud from `0x100000` with a 4800-baud cold init; write at 19200 baud with 128-byte XOR-`0x82` blocks. **Operator flow change:** the "apply VPP" prompt now comes before the connection opens instead of mid-session, and a "remove VPP" notice follows every write outcome, not only success. **Deliberate corrections:** a rejected enter-flash-mode request fails before programming voltage is raised or erase is sent; cancellation reports cancelled instead of success; both erase polls fail when exhausted (legacy programmed after the second poll timed out); the reply to the final `AF 69` block is read and a malformed or negative reply fails; the write image must be exactly the ROM size; read pages must be complete, checksummed 128-byte `E0` frames; a stale ISO-14230 header is cleared before configuration. Silence after `AF 69` is accepted with a warning; see the [bench checklist](unisia-jecs-m32r-bench-checklist.md). No hardware qualification is claimed. |
```

- [ ] **Step 2: Write the bench checklist**

`docs/unisia-jecs-m32r-bench-checklist.md`:

```markdown
# Subaru Unisia Jecs M32R K-Line bench checklist

## 0. STOP — not hardware-qualified

Do not treat this family as proven until every section below has passed on
real hardware. Record the adapter (make, model, firmware; OpenPort 2.0 or
not), the VPP supply when the adapter does not provide it, the ECU part
number and ECU ID, the protocol selected, date, operator and result.

## 1. Read

- Read the full ROM with each protocol the bench ECU supports and compare it
  byte-for-byte with a trusted dump of the same ECU.
- Confirm the saved ROM ID is the ECU ID followed by `_`.
- Repeat with the ECU already in read mode (a second read without cycling
  ignition) and confirm the cold init is skipped.
- Pull the K-Line cable mid-read and confirm the operation fails instead of
  returning a short image.

## 2. Write (`_20` / `_30` only, on a bench ECU with a recovery path)

- Without an OpenPort 2.0: confirm the "apply VPP" prompt appears before the
  connection opens, and that declining it stops before any K-Line traffic.
- Capture a trace of one full write. Record the exact bytes the ECU sends
  after the final `AF 69` block, or confirm it sends nothing. The executor
  currently accepts `EF 52` or silence; tighten it to what the trace shows.
- Confirm the `EF 42` (erase started) and `EF 52` (erased) replies arrive
  within the 20- and 40-round poll budgets, and record how long each took.
- Read the ROM back and compare it with the written image.
- Confirm the "remove VPP" notice appears after success, and after a write
  cancelled during programming.
```

- [ ] **Step 3: Update the specs and the modularization plan**

In `docs/superpowers/specs/2026-09-25-step5-tail-wave6c3-unisia-jecs-m32r-kline-design.md`:
- change the status line to `**Status:** design approved; implementation plan in [the 6c-3 plan](../plans/2026-09-25-step5-tail-wave6c3-unisia-jecs-m32r-kline.md).`
- replace ``- A new bench checklist, `docs/unisia-jecs-m32r-bench-checklist.md`, which
  includes observing the `AF 69` reply.`` with `- A new [bench checklist](../../unisia-jecs-m32r-bench-checklist.md), which includes observing the `AF 69` reply.`

In `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md`:
- change the status line to `**Status:** in progress — waves 6a-1 through 6c-2 are merged; 6c-3 is implemented on this branch. One legacy family remains, for wave 7.`
- append:

```markdown
## Wave 6c-3 implementation note

The [Unisia Jecs M32R K-Line family spec](2026-09-25-step5-tail-wave6c3-unisia-jecs-m32r-kline-design.md)
records three departures from this design: the 6a-3/6a-4/6b-2/6c-1
behavior-correction exception applies, because legacy could erase under VPP
after a rejected flash-mode request and reported cancellation as success; the
VPP flow gains a post-attempt `RemoveProgrammingVoltage` prompt alongside the
planned `ApplyProgrammingVoltage` confirmation; and the legacy dialog package
`//src/ui/desktop/flash/ecu` is deleted, taking its entry out of the
`serial_qt_compat` allowlist. The drain moves from two entries to one;
`FlashEcuSubaruUnisiaJecsM32rBootMode` remains for wave 7.
```

In `docs/modularization-plan.md`:
- replace the `- Wave 6c-2 ...` bullet (four lines) with:

```markdown
- Wave 6c-2 `FlashEcuSubaruHitachiM32rJtag` — removed, not migrated (#358):
  it was unreachable and its read and write were stubs. See the
  [removal design](superpowers/specs/2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal-design.md).
- Wave 6c-3 `FlashEcuSubaruUnisiaJecsM32r` — implemented on this branch. The
  drain is one remaining family, the wave-7 bootmode. Hardware status remains
  experimental. See the
  [family design](superpowers/specs/2026-09-25-step5-tail-wave6c3-unisia-jecs-m32r-kline-design.md).
```
- in step 5's heading, change `flash-tail Wave 6c-2 implemented on this branch, with 2 legacy families remaining` to `flash-tail Wave 6c-3 implemented on this branch, with 1 legacy family remaining`.

- [ ] **Step 4: Run all gates**

```bash
bazel test --config=release //...
bazel build --config=release //:fastecu //:portable_closure
prek run --all-files
bazel run //:clang_tidy_report_changed
python3 scripts/check-legacy-flash-drain.py
scripts/coverage-local.sh   # confirm >=80% on the new executor/plan sources
```
Expected: all green; 1 family remaining; clang-tidy reports no findings in changed files. Fix coverage gaps with tests, not exclusions. If `//tests:serial_backend_tests` fails only on Windows CI, it is a known pre-existing flake: rerun it rather than debugging this branch.

- [ ] **Step 5: Commit**

```bash
git add docs
git commit -m "docs(flash): record the Unisia Jecs M32R K-Line migration"
```

- [ ] **Step 6: Ask the user for approval to push, then open the PR**

After approval:
```bash
git push -u origin feat/wave6c3-unisia-jecs-m32r-kline
gh pr create --title "feat(flash): migrate Subaru Unisia Jecs M32R K-Line (wave 6c-3)" --body "$(cat <<'EOF'
### What
- portable plan and executor for `sub_ecu_unisia_jecs_{20,30,40,70}`: framed SSM ROM read on all four, and erase/program write on `_20` and `_30`
- `ApplyProgrammingVoltage` confirmation before the attempt and a `RemoveProgrammingVoltage` notice after every write outcome, both skipped when the adapter supplies VPP
- route through the portable desktop workflow; remove the legacy dialog package, operation and MainWindow branches
- drain ratchet two -> one; `serial_qt_compat` allowlist shrinks by one; matrix row `experimental`; bench checklist

### Why
- the last wave-6 family of the step 5 legacy flash drain; only the wave-7 bootmode remains
- legacy could raise VPP and erase after the ECU rejected flash mode, program after the erase poll timed out, and report a cancelled write as a success
- corrections under the 6a-3/6a-4/6b-2/6c-1 policy: every reply gated, cancellation reported, exact-size images, checksummed read pages, programming-voltage line dropped on every exit

### Verification
- bazel test --config=release //...
- bazel build --config=release //:fastecu //:portable_closure
- prek run --all-files; bazel run //:clang_tidy_report_changed
- mutation checks for every correction

### References
- design: docs/superpowers/specs/2026-09-25-step5-tail-wave6c3-unisia-jecs-m32r-kline-design.md
- plan: docs/superpowers/plans/2026-09-25-step5-tail-wave6c3-unisia-jecs-m32r-kline.md
- hardware qualification blocked by docs/unisia-jecs-m32r-bench-checklist.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc
EOF
)"
```
