# Wave 6c-1 — Subaru Denso MC68HC16Y5 BDM — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate `FlashEcuSubaruDensoMC68HC16Y5_02_BDMOperation` to a portable plan and `IKlineFlashExecutor` routed through `FlashWorkflow`, taking `//:legacy_flash_drain` from four entries to three.

**Architecture:** A family plan accepts only `sub_ecu_denso_mc68hc16y5_02_bdm` / `MC68HC16Y5`. Read covers the `0x0–0x2FFFF` address space, and Write carries the zero-padded cfg kernel as its `image` at `0x20000`. The executor speaks the BDM bridge's ASCII protocol at 115200 baud through `write_raw()` / `read_raw()` only. It accumulates replies across reads and gates every `ACK_CMD_WDMEM` / `ACK_WR`. A new desktop workflow adds a `ConfirmBdmKernelBootstrap` prompt for Write, replaces the `Unrouted` reservation, and lets the legacy dialog, operation and `MainWindow` branch be deleted.

**Tech Stack:** C++23, Bazel, GoogleTest (`fastecu_portable_gtest`), QtTest for the desktop workflow suite.

**Spec:** [Wave 6c-1 — Denso MC68HC16Y5 BDM — Design](../specs/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm-design.md). Parent: [Wave 6 singletons](../specs/2026-09-19-step5-tail-wave6-singletons-design.md).

## Global Constraints

- Behavior-correction policy is the 6a-3/6a-4/6b-2 exception. Command bytes, sequencing and timing budgets are preserved. Read-integrity and reply-gating defects are corrected, and each correction is named in the matrix notes. The `wpcsp`/`go` replies stay ungated (spec appendix).
- One PR. No port addition: `write_raw()` / `read_raw()` landed in #351.
- The executor calls only `write_raw()` and `read_raw()` for traffic, never `write()` or `read()`. It never calls `configure()`, `open()` or `close()` (ADR 0015). The ISO-14230 header is cleared in `before_transport_configure()`.
- Write never carries or touches the operator's ROM. The plan's `image` is the padded kernel; `kernel()` is empty; `family_requires_kernel_v` is `false`.
- Backend operations return `fastecu::Result<T>`, checked with `.has_value()`, never implicit `operator bool`. Exceptions never cross a port. Do not add an `ErrorKind`.
- Pure protocol code uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView`. The backend never sees `EcuCalDefStructure`, Qt, threads or the filesystem.
- New backend targets live in `src/backend/flash/ecu/` and are registered by name under `"src/backend/flash/ecu"` in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`).
- Add no ratchet entry. Remove exactly `bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.cpp` from `REMAINING` in `scripts/check-legacy-flash-drain.py`.
- Executor steps carry a comment citing the legacy function and line in `src/platform/desktop/common/flash/legacy/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.cpp`. That file exists until Task 4 deletes it; read the cited lines while it does.
- Every test that pins a correction is mutation-checked. Change the production line, run the named test, watch it fail, then restore the file byte-identically (`git diff --exit-code <file>`). The mutations are listed in the Task 2 and Task 3 steps.
- Tests are package-owned and co-located. Mocks and fakes stay package-owned.
- Markdown cross-references are links with human-readable text, not backticked paths.
- Work lands through pull requests. Do not push or open a PR without explicit user approval.

## Legacy constants

| Name | Value | Legacy source |
|---|---|---|
| baud | 115200 | `execute()` :54 |
| `serial_read_short_timeout` | 200 ms | header |
| `serial_read_long_timeout` | 800 ms | header |
| `serial_read_extra_long_timeout` | 3000 ms | header |
| read page | 0x400 | `read_mem()` :93 |
| page poll budget | 50 rounds × (200 ms read, 100 ms sleep) | `read_mem()` :157-163 |
| inter-page delay | 1 ms | `read_mem()` :204 |
| RAM hole | `0x20000–0x27FFF`, filled `0xFF` | `read_mem()` :132-144, `rblocks_MC68HC16Y5` |
| upload chunk | 0x20, kernel zero-padded to a multiple | `write_mem()` :247-250, `flash_block()` :361 |
| SCIB enable | `wdmem 0xFFC28 0x4` then `00 0D 00 0C` | `write_mem()` :258-294 |

## File map

- Create `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_types.h`
- Create `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.{h,cpp}` + `_plan_test.cpp`
- Create `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.{h,cpp}` + `_executor_test.cpp`
- Modify `src/backend/flash/ecu/BUILD.bazel`, `src/backend/flash/BUILD.bazel`, `src/backend/flash/flash_types.h`, `src/backend/flash/flash_plan.cpp`, `src/backend/flash/testing/flash_printers.h`, `src/backend/flash/flash_validation_test.cpp`, `bazel/portable_targets.bzl`
- Modify `src/platform/desktop/common/flash/flash_workflow.{h,cpp}`, `flash_workflow_test.cpp`, `src/platform/desktop/common/flash/BUILD.bazel`
- Modify `src/ui/desktop/flash/common/flash_dialog.cpp`, `src/ui/desktop/mainwindow.{h,cpp}`, `src/ui/desktop/BUILD.bazel`
- Modify `src/platform/desktop/common/flash/legacy/BUILD.bazel`, `scripts/check-legacy-flash-drain.py`
- Delete `src/ui/desktop/flash/bdm/` and `src/platform/desktop/common/flash/legacy/bdm/`
- Modify `docs/flash-qualification-matrix.md`, `docs/modularization-plan.md`, both specs; create `docs/denso-mc68hc16-bdm-bench-checklist.md`

The branch `feat/wave6c1-denso-mc68hc16-bdm` already exists, based on `d345a8fa`, and carries the spec commit.

---

### Task 1: Plan, family registration, validation

**Files:**
- Create: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_types.h`, `subaru_denso_mc68hc16y5_02_bdm_plan.h`, `subaru_denso_mc68hc16y5_02_bdm_plan.cpp`, `subaru_denso_mc68hc16y5_02_bdm_plan_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `src/backend/flash/BUILD.bazel:39`, `src/backend/flash/flash_types.h`, `src/backend/flash/flash_plan.cpp:56-60`, `src/backend/flash/testing/flash_printers.h:106-111`, `src/backend/flash/flash_validation_test.cpp:213-220`, `bazel/portable_targets.bzl:118-120`

**Interfaces:**
- Produces: `struct SubaruDensoMc68hc16y5_02BdmPlan { int baud; };`, `FlashFamily::SubaruDensoMc68hc16y5_02Bdm`,
  `Result<FlashPlan> build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation operation, std::string_view protocol_name, std::string_view mcu_type, std::optional<bytes::Bytes> rom_image, std::optional<KernelImage> kernel);`,
  `Status validate_subaru_denso_mc68hc16y5_02_bdm_plan(const FlashPlan& plan);`
- Plan shape: Read → transfer `{0, 0x30000}`, no image. Write → transfer `{0x20000, padded}`, `image` = kernel zero-padded to a multiple of `0x20`, at most `0x8000`. Both → `kernel()` empty, no erase regions, no confirmations, `baud == 115200`.

- [ ] **Step 1: Register the family so tests can compile**

`src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_types.h`:

```cpp
#pragma once

namespace fastecu::flash
{
// Step 5 tail, wave 6c-1. The BDM bridge speaks a fixed-baud ASCII command
// protocol; the baud is the only wire parameter.
struct SubaruDensoMc68hc16y5_02BdmPlan
{
    int baud;
};
} // namespace fastecu::flash
```

In `src/backend/flash/flash_types.h`:
- add `#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_types.h"` after the `subaru_denso_mc68hc16y5_02_types.h` include;
- append to `enum class FlashFamily`, after `SubaruDensoSh705xKline,`:
  ```cpp
      // Step 5 tail, wave 6c-1.
      SubaruDensoMc68hc16y5_02Bdm,
  ```
- append `, SubaruDensoMc68hc16y5_02BdmPlan` as the last `FamilyPlan` alternative (after `SubaruDensoSh705xKlinePlan`);
- after the `FamilyTraits<SubaruDensoSh705xKlinePlan>` specialization add:
  ```cpp
  template <> struct FamilyTraits<SubaruDensoMc68hc16y5_02BdmPlan>
  {
      static constexpr FlashFamily family = FlashFamily::SubaruDensoMc68hc16y5_02Bdm;
      static constexpr TransportKind transport = TransportKind::Kline;
  };
  ```
- after `family_requires_kernel_v<SubaruUnisiaJecsPlan> = false;` add:
  ```cpp
  // Step 5 tail, wave 6c-1. Write uploads the cfg kernel over BDM, but carries
  // it as the plan image (the bytes written to RAM), not as a KernelImage.
  template <> inline constexpr bool family_requires_kernel_v<SubaruDensoMc68hc16y5_02BdmPlan> = false;
  ```

In `src/backend/flash/flash_plan.cpp`, after the `SubaruDensoSh705xKline` case:
```cpp
    case FlashFamily::SubaruDensoMc68hc16y5_02Bdm:
        return "SubaruDensoMc68hc16y5_02Bdm";
```

In `src/backend/flash/testing/flash_printers.h`, after the `SubaruDensoSh705xKline` case:
```cpp
    case FlashFamily::SubaruDensoMc68hc16y5_02Bdm:
        *os << "SubaruDensoMc68hc16y5_02Bdm";
        return;
```

In `src/backend/flash/BUILD.bazel`, add `"//src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_types",` to the list that already holds `"//src/backend/flash/ecu:subaru_unisia_jecs_types",` (keep it sorted as its neighbours are).

In `src/backend/flash/flash_validation_test.cpp`, append to the family table after the `SubaruDensoSh705xKline` row:
```cpp
        {FlashFamily::SubaruDensoMc68hc16y5_02Bdm, TransportKind::Kline, SubaruDensoMc68hc16y5_02BdmPlan{.baud = 115200},
         "SubaruDensoMc68hc16y5_02Bdm"},
```

- [ ] **Step 2: Write the failing plan tests**

`src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h`:

```cpp
#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
// Read returns the 0x0-0x2FFFF address-space image. Write is a kernel
// bootstrap: `kernel` is uploaded to RAM and started; the ROM is never
// written, so `rom_image` must be absent for every operation.
Result<FlashPlan> build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type,
                                                           std::optional<bytes::Bytes> rom_image,
                                                           std::optional<KernelImage> kernel);
Status validate_subaru_denso_mc68hc16y5_02_bdm_plan(const FlashPlan& plan);
} // namespace fastecu::flash
```

`src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

constexpr std::string_view kProtocol = "sub_ecu_denso_mc68hc16y5_02_bdm";
constexpr std::string_view kMcu = "MC68HC16Y5";

KernelImage kernel(bytes::Bytes content, std::uint32_t load_address = 0x20000)
{
    return KernelImage{.id = "bdm-kernel", .load_address = load_address, .bytes = std::move(content)};
}

FlashPlanFields read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruDensoMc68hc16y5_02Bdm,
        .transport = TransportKind::Kline,
        .target_id = std::string(kProtocol),
        .mcu_name = std::string(kMcu),
        .transfer_region = MemoryRegion{0, 0x30000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = 115200},
        .confirmations = {},
    };
}

FlashPlanFields write_fields(std::uint32_t size)
{
    FlashPlanFields fields = read_fields();
    fields.operation = FlashOperation::Write;
    fields.transfer_region = MemoryRegion{0x20000, size};
    fields.image = bytes::Bytes(size, 0xab);
    return fields;
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ReadCoversTheAddressSpaceImage)
{
    const auto plan = build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt,
                                                               std::nullopt);
    ASSERT_THAT(plan, IsOk());
    EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoMc68hc16y5_02Bdm);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);
    EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, 0x30000}));
    EXPECT_FALSE(plan->image().has_value());
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_EQ(std::get<SubaruDensoMc68hc16y5_02BdmPlan>(plan->family_plan()).baud, 115200);
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WritePadsTheKernelAndCarriesItAsTheImage)
{
    const auto plan = build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                               kernel(bytes::Bytes(33, 0xab)));
    ASSERT_THAT(plan, IsOk());
    ASSERT_TRUE(plan->image().has_value());
    const bytes::Bytes& image = *plan->image();
    ASSERT_EQ(image.size(), 64U);
    EXPECT_TRUE(std::all_of(image.begin(), image.begin() + 33, [](bytes::Byte value) { return value == 0xab; }));
    EXPECT_TRUE(std::all_of(image.begin() + 33, image.end(), [](bytes::Byte value) { return value == 0x00; }));
    EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0x20000, 64}));
    EXPECT_FALSE(plan->kernel().has_value());
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WriteAcceptsAKernelThatFillsTheRamBlockExactly)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x8000, 0x01))),
                IsOk());
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x8001, 0x01))),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsEveryOtherIdentity)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_denso_mc68hc16y5_02_bdm", "MC68HC16Y5_TPU"},
             {"sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5"},
             {"sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5_TPU"},
             {"sub_ecu_denso_mc68hc16y5_02_bdm_x", "MC68HC16Y5"},
         }))
    {
        EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, protocol, mcu, std::nullopt,
                                                              std::nullopt),
                    IsErr(ErrorKind::InvalidConfig))
            << protocol << " / " << mcu;
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsTestWrite)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::TestWrite, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::Unsupported));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsARomImageForEveryOperation)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu,
                                                          bytes::Bytes(0x30000, 0x00), std::nullopt),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu,
                                                          bytes::Bytes(0x30000, 0x00), kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ReadRejectsAKernel)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WriteRejectsAMissingEmptyOrMisplacedKernel)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          std::nullopt),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes{})),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x20, 0x01), 0x21000)),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ValidatorRejectsHandBuiltShapes)
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
        fields.family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = 9600};
        cases.push_back({"wrong baud", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.kernel = kernel(bytes::Bytes(0x20, 0x01));
        cases.push_back({"read with kernel", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.transfer_region = MemoryRegion{0, 0x28000};
        cases.push_back({"packed read region", std::move(fields), ErrorKind::InvalidConfig});
    }
    cases.push_back({"unaligned image", write_fields(0x21), ErrorKind::InvalidConfig});
    cases.push_back({"oversize image", write_fields(0x8020), ErrorKind::InvalidConfig});
    {
        auto fields = write_fields(0x20);
        fields.transfer_region = MemoryRegion{0x20000, 0x40};
        cases.push_back({"region/image mismatch", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields(0x20);
        fields.transfer_region = MemoryRegion{0, 0x20};
        cases.push_back({"image outside RAM", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields(0x20);
        fields.operation = FlashOperation::TestWrite;
        cases.push_back({"test write", std::move(fields), ErrorKind::Unsupported});
    }

    for (auto& test_case : cases)
    {
        auto plan = validate_and_build(std::move(test_case.fields));
        ASSERT_THAT(plan, IsOk()) << test_case.name;
        EXPECT_THAT(validate_subaru_denso_mc68hc16y5_02_bdm_plan(*plan), IsErr(test_case.expected)) << test_case.name;
    }
}
} // namespace
} // namespace fastecu::flash
```

Add to `src/backend/flash/ecu/BUILD.bazel`, after the `subaru_unisia_jecs_executor_test` target:

```starlark
cc_library(
    name = "subaru_denso_mc68hc16y5_02_bdm_types",
    hdrs = ["subaru_denso_mc68hc16y5_02_bdm_types.h"],
)

cc_library(
    name = "subaru_denso_mc68hc16y5_02_bdm_plan",
    srcs = ["subaru_denso_mc68hc16y5_02_bdm_plan.cpp"],
    hdrs = ["subaru_denso_mc68hc16y5_02_bdm_plan.h"],
    deps = [
        ":subaru_denso_mc68hc16y5_02_bdm_types",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/flash:flash_plan",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_denso_mc68hc16y5_02_bdm_plan_test",
    srcs = ["subaru_denso_mc68hc16y5_02_bdm_plan_test.cpp"],
    deps = [
        ":subaru_denso_mc68hc16y5_02_bdm_plan",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

In `bazel/portable_targets.bzl`, under `"src/backend/flash/ecu"`, after `"subaru_unisia_jecs_executor",` add:
```starlark
        "subaru_denso_mc68hc16y5_02_bdm_types",
        "subaru_denso_mc68hc16y5_02_bdm_plan",
```

Create `subaru_denso_mc68hc16y5_02_bdm_plan.cpp` holding only the includes and namespace, so the target links and the tests fail on missing symbols:

```cpp
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
```

- [ ] **Step 3: Run to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_plan_test`
Expected: FAIL to link, with undefined `build_subaru_denso_mc68hc16y5_02_bdm_plan` / `validate_subaru_denso_mc68hc16y5_02_bdm_plan`.

- [ ] **Step 4: Implement the plan**

Replace `subaru_denso_mc68hc16y5_02_bdm_plan.cpp` with:

```cpp
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"

#include <cstddef>
#include <format>
#include <string_view>
#include <utility>

#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
constexpr std::string_view kProtocol = "sub_ecu_denso_mc68hc16y5_02_bdm";
constexpr std::string_view kMcu = "MC68HC16Y5";
// Legacy execute() :54.
constexpr int kBaud = 115200;
// Legacy read_mem() walks the whole 0x0-0x2FFFF address space, filling the
// RAM block with 0xFF (:116-144), so the read image is physical, not packed.
constexpr MemoryRegion kReadRegion{0, 0x30000};
constexpr MemoryRegion kRam{0x20000, 0x8000};
constexpr std::uint32_t kRomSize = 0x28000;
// Legacy write_mem() pads the kernel to 32 bytes (:247-250) and flash_block()
// uploads it in 32-byte chunks (:361).
constexpr std::size_t kUploadChunk = 0x20;

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol || mcu != kMcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("MC68HC16Y5 BDM protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const flashdev_t *device = find_flash_device(mcu);
    if (device == nullptr || device->romsize != kRomSize || device->rblocks == nullptr ||
        device->rblocks[0].start != kRam.start || device->rblocks[0].len != kRam.length)
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM memory map is invalid");
    }
    return {};
}

bytes::Bytes pad_kernel(bytes::Bytes kernel)
{
    kernel.resize((kernel.size() + kUploadChunk - 1) / kUploadChunk * kUploadChunk, 0x00);
    return kernel;
}
} // namespace

Status validate_subaru_denso_mc68hc16y5_02_bdm_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruDensoMc68hc16y5_02Bdm || plan.transport() != TransportKind::Kline)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Denso MC68HC16Y5 BDM");
    }
    if (auto identity = validate_identity(plan.target_id(), plan.mcu_name()); !identity.has_value())
    {
        return identity;
    }
    if (plan.operation() == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "MC68HC16Y5 BDM has no test write");
    }
    const auto *wire = std::get_if<SubaruDensoMc68hc16y5_02BdmPlan>(&plan.family_plan());
    if (wire == nullptr || wire->baud != kBaud)
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM wire parameters are invalid");
    }
    if (!plan.erase_regions().empty() || plan.kernel().has_value() || !plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM plan shape is invalid");
    }
    if (plan.operation() == FlashOperation::Read)
    {
        if (plan.transfer_region() != kReadRegion || plan.image().has_value())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM read-plan shape is invalid");
        }
        return {};
    }
    const auto& image = plan.image();
    if (!image.has_value() || image->empty() || image->size() % kUploadChunk != 0 || image->size() > kRam.length ||
        plan.transfer_region() != MemoryRegion{kRam.start, static_cast<std::uint32_t>(image->size())})
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM kernel-bootstrap plan shape is invalid");
    }
    return {};
}

Result<FlashPlan> build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type,
                                                           std::optional<bytes::Bytes> rom_image,
                                                           std::optional<KernelImage> kernel)
{
    if (auto identity = validate_identity(protocol_name, mcu_type); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "MC68HC16Y5 BDM has no test write");
    }
    if (rom_image.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM plans never carry a ROM image");
    }

    MemoryRegion region = kReadRegion;
    std::optional<bytes::Bytes> image;
    if (operation == FlashOperation::Read)
    {
        if (kernel.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM read plans must not carry a kernel");
        }
    }
    else
    {
        if (!kernel.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM kernel bootstrap requires a kernel image");
        }
        if (kernel->load_address != kRam.start)
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("MC68HC16Y5 BDM kernel must load at 0x{:X}, not 0x{:X}", kRam.start,
                                    kernel->load_address));
        }
        if (kernel->bytes.empty())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM kernel image is empty");
        }
        bytes::Bytes padded = pad_kernel(std::move(kernel->bytes));
        if (padded.size() > kRam.length)
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("MC68HC16Y5 BDM kernel ({} bytes padded) exceeds the 0x{:X}-byte RAM block",
                                    padded.size(), kRam.length));
        }
        region = MemoryRegion{kRam.start, static_cast<std::uint32_t>(padded.size())};
        image = std::move(padded);
    }

    auto plan = validate_and_build(FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::SubaruDensoMc68hc16y5_02Bdm,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = region,
        .erase_regions = {},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = kBaud},
        .confirmations = {},
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_denso_mc68hc16y5_02_bdm_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
```

- [ ] **Step 5: Run to verify they pass**

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_plan_test //src/backend/flash:all
bazel build --config=release //:portable_closure
```
Expected: PASS. `//src/backend/flash:all` includes `flash_validation_test`, which exercises the new table row.

- [ ] **Step 6: Commit**

```bash
git add bazel/portable_targets.bzl src/backend/flash
git commit -m "feat(flash): add the Denso MC68HC16Y5 BDM plan"
```

---

### Task 2: Executor — setup, header seam, reply accumulation, Read

**Files:**
- Create: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h`, `subaru_denso_mc68hc16y5_02_bdm_executor.cpp`, `subaru_denso_mc68hc16y5_02_bdm_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `build_subaru_denso_mc68hc16y5_02_bdm_plan`, `validate_subaru_denso_mc68hc16y5_02_bdm_plan` (Task 1); `IKlineFlashTransport::write_raw`, `read_raw`, `set_add_iso14230_header`; `ScriptedKlineFlashTransport::expectRawWrite`, `queueRawRead`, `header_mode_calls_`, `read_timeouts_`, `writesConsumed()`, `scriptConsumed()`.
- Produces: `class SubaruDensoMc68hc16y5_02BdmExecutor final : public IKlineFlashExecutor` with `transport_setup`, `before_transport_configure`, `execute`. Anonymous-namespace helpers in the `.cpp` that Task 3 reuses: `ascii`, `cancelled_if_requested`, `write_exact`, `accumulate`, `discard`, `expect_ack`, and the timeout constants `kShortTimeout`, `kLongTimeout`, `kExtraLongTimeout`.

- [ ] **Step 1: Write the failing executor tests**

`src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h`:

```cpp
#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruDensoMc68hc16y5_02BdmExecutor final : public IKlineFlashExecutor
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

`src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
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

constexpr std::string_view kProtocol = "sub_ecu_denso_mc68hc16y5_02_bdm";
constexpr std::string_view kMcu = "MC68HC16Y5";

// Fails the test on any framed call: every BDM exchange must be raw.
class RawOnlyTransport final : public ScriptedKlineFlashTransport
{
  public:
    Result<std::size_t> write(bytes::ByteView data) override
    {
        ++framed_calls;
        return ScriptedKlineFlashTransport::write(data);
    }
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        ++framed_calls;
        return ScriptedKlineFlashTransport::read(timeout, cancellation);
    }

    int framed_calls = 0;
};

bytes::Bytes ascii(std::string_view text)
{
    bytes::Bytes out;
    for (const char c : text)
    {
        out.push_back(static_cast<bytes::Byte>(c));
    }
    return out;
}

void nothing(ScriptedKlineFlashTransport& transport)
{
    transport.queueRawRead(bytes::Bytes{});
}

std::vector<std::uint32_t> page_addresses()
{
    std::vector<std::uint32_t> addresses;
    for (std::uint32_t address = 0; address < 0x20000; address += 0x400)
    {
        addresses.push_back(address);
    }
    for (std::uint32_t address = 0x28000; address < 0x30000; address += 0x400)
    {
        addresses.push_back(address);
    }
    return addresses;
}

bytes::Bytes page_bytes(std::uint32_t address)
{
    return bytes::Bytes(0x400, static_cast<bytes::Byte>((address >> 10) ^ 0x5a));
}

bytes::Bytes rpmem(std::uint32_t address)
{
    return ascii(std::format("rpmem 0x{:08X} 0x00000400", address));
}

FlashPlan read_plan()
{
    auto plan = build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt,
                                                         std::nullopt);
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

// Scripts the full 160-page read; when `split_first_page` is set the first
// page arrives in two separate polls, 0x100 bytes then 0x300 bytes, with an
// empty read ending the first poll. Legacy replaced its buffer on each poll,
// so only accumulation across polls yields the whole page.
void script_read(ScriptedKlineFlashTransport& transport, bool split_first_page = false)
{
    nothing(transport); // read_mem() :113 clears the receive buffer
    for (const std::uint32_t address : page_addresses())
    {
        transport.expectRawWrite(rpmem(address));
        const bytes::Bytes page = page_bytes(address);
        if (split_first_page && address == 0)
        {
            transport.queueRawRead(bytes::ByteView(page).first(0x100));
            nothing(transport);
            transport.queueRawRead(bytes::ByteView(page).subspan(0x100));
        }
        else
        {
            transport.queueRawRead(page);
        }
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, TransportSetupIs115200BaudPlainSerial)
{
    const auto setup = SubaruDensoMc68hc16y5_02BdmExecutor{}.transport_setup(read_plan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 115200);         // execute() :54
    EXPECT_FALSE(setup->iso14230);          // execute() :50
    EXPECT_EQ(setup->tester_id, 0);
    EXPECT_EQ(setup->target_id, 0);
    EXPECT_EQ(setup->parity, KlineParity::None);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, BeforeConfigureClearsTheIso14230Header)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruDensoMc68hc16y5_02BdmExecutor{}.before_transport_configure(transport, clock, cancellation),
                IsOk());
    EXPECT_EQ(transport.header_mode_calls_, std::vector<bool>{false});
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ReadsTheAddressSpaceImageWithTheRamHoleFilled)
{
    RawOnlyTransport transport;
    script_read(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& image = *result->read_bytes;
    ASSERT_EQ(image.size(), 0x30000U);
    for (const std::uint32_t address : page_addresses())
    {
        ASSERT_TRUE(std::equal(image.begin() + address, image.begin() + address + 0x400,
                               page_bytes(address).begin()))
            << std::format("page 0x{:05X}", address);
    }
    EXPECT_TRUE(std::all_of(image.begin() + 0x20000, image.begin() + 0x28000,
                            [](bytes::Byte value) { return value == 0xff; }));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.framed_calls, 0);
    EXPECT_EQ(transport.read_timeouts_.front(), 200ms);
    EXPECT_EQ(events.progress_calls.back(), (std::pair<int, int>{160, 160}));
    // Each page: one 100 ms poll sleep (read_mem() :162) and 1 ms (:204).
    EXPECT_EQ(clock.elapsed(), 160 * 101ms);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AssemblesAPageDeliveredAcrossPolls)
{
    ScriptedKlineFlashTransport transport;
    script_read(transport, true);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(std::equal(result->read_bytes->begin(), result->read_bytes->begin() + 0x400, page_bytes(0).begin()));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ShortPageFailsAfterFiftyPolls)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(rpmem(0));
    transport.queueRawRead(bytes::Bytes(0x10, 0x01));
    for (int poll = 0; poll < 50; ++poll)
    {
        nothing(transport);
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, OverLongPageFailsBeforeTheNextCommand)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(rpmem(0));
    transport.queueRawRead(bytes::Bytes(0x401, 0x01));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationBetweenPagesStopsBeforeTheNextCommand)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(rpmem(0));
    transport.queueRawRead(page_bytes(0));
    FakeClock clock;
    FakeCancellationToken cancellation;
    // Trips once the buffer clear and the first page have been read.
    cancellation.set_predicate([&transport] { return transport.read_timeouts_.size() >= 2; });
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(read_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}
} // namespace
} // namespace fastecu::flash
```

Add to `src/backend/flash/ecu/BUILD.bazel`, after the Task 1 targets:

```starlark
cc_library(
    name = "subaru_denso_mc68hc16y5_02_bdm_executor",
    srcs = ["subaru_denso_mc68hc16y5_02_bdm_executor.cpp"],
    hdrs = ["subaru_denso_mc68hc16y5_02_bdm_executor.h"],
    deps = [
        ":subaru_denso_mc68hc16y5_02_bdm_plan",
        "//src/algorithms/protocol",
        "//src/backend/flash:flash_executor",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_denso_mc68hc16y5_02_bdm_executor_test",
    srcs = ["subaru_denso_mc68hc16y5_02_bdm_executor_test.cpp"],
    deps = [
        ":subaru_denso_mc68hc16y5_02_bdm_executor",
        ":subaru_denso_mc68hc16y5_02_bdm_plan",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:recording_event_sink",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

In `bazel/portable_targets.bzl`, after `"subaru_denso_mc68hc16y5_02_bdm_plan",` add `"subaru_denso_mc68hc16y5_02_bdm_executor",`.

Create `subaru_denso_mc68hc16y5_02_bdm_executor.cpp` containing only `#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"`.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_executor_test`
Expected: FAIL to link (undefined executor methods).

- [ ] **Step 3: Implement setup, helpers and Read**

Replace `subaru_denso_mc68hc16y5_02_bdm_executor.cpp` with:

```cpp
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

// Legacy header: serial_read_short/long/extra_long_timeout.
constexpr auto kShortTimeout = 200ms;
constexpr auto kLongTimeout = 800ms;
constexpr auto kExtraLongTimeout = 3000ms;
// read_mem() :157-163 polls up to 50 times, sleeping 100 ms after each poll,
// then sleeps 1 ms per page (:204).
constexpr unsigned kPagePolls = 50;
constexpr auto kPagePollDelay = 100ms;
constexpr auto kInterPageDelay = 1ms;
constexpr std::uint32_t kPageSize = 0x400;
constexpr std::uint32_t kRamStart = 0x20000;
constexpr std::uint32_t kRamEnd = 0x28000;
constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();

bytes::Bytes ascii(std::string_view text)
{
    bytes::Bytes out;
    out.reserve(text.size());
    for (const char c : text)
    {
        out.push_back(static_cast<bytes::Byte>(c));
    }
    return out;
}

Status cancelled_if_requested(const ICancellationToken& cancellation)
{
    return cancellation.cancelled() ? fail(ErrorKind::Cancelled, "MC68HC16Y5 BDM operation cancelled") : Status{};
}

Status write_exact(IKlineFlashTransport& transport, bytes::ByteView request)
{
    auto written = transport.write_raw(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return *written == request.size() ? Status{} : fail(ErrorKind::Disconnected, "short BDM write");
}

// Reads until `want` bytes have arrived, `budget` has elapsed, or a read
// returns nothing. read_raw() returns nothing only after waiting out the whole
// timeout it was given, so an empty read means the window is spent. Legacy
// read_serial_data() instead ran every reply through the K-Line frame parser,
// which cannot pass ASCII or 1 KiB pages (spec: deliberate corrections).
Result<bytes::Bytes> accumulate(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                                std::size_t want, std::chrono::milliseconds budget)
{
    const auto deadline = clock.now() + budget;
    bytes::Bytes received;
    while (received.size() < want)
    {
        if (auto cancelled = cancelled_if_requested(cancellation); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        const auto now = clock.now();
        if (now >= deadline)
        {
            break;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        auto chunk = transport.read_raw(remaining, cancellation);
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (!chunk->has_value() || (*chunk)->empty())
        {
            break;
        }
        received.insert(received.end(), (*chunk)->begin(), (*chunk)->end());
    }
    return received;
}

Status discard(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
               IEventSink& events, std::chrono::milliseconds budget)
{
    auto drained = accumulate(transport, clock, cancellation, kUnbounded, budget);
    if (!drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    if (!drained->empty())
    {
        events.log(LogLevel::Debug, std::format("BDM discarded: {}", bytes::toHex(*drained)));
    }
    return {};
}

// Legacy compares the whole reply with the token (flash_block() :394,
// write_mem() :268, :286); the accumulated reply must equal it exactly.
Status expect_ack(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                  std::string_view token, std::chrono::milliseconds budget)
{
    auto received = accumulate(transport, clock, cancellation, token.size(), budget);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (*received == ascii(token))
    {
        return {};
    }
    if (received->size() < token.size())
    {
        return fail(ErrorKind::Timeout,
                    std::format("BDM bridge did not send {} (received {})", token, bytes::toHex(*received)));
    }
    return fail(ErrorKind::BadResponse,
                std::format("BDM bridge sent {} instead of {}", bytes::toHex(*received), token));
}

// read_mem() :146-175. Legacy replaced its buffer on every poll and appended
// any non-empty remainder; the page now accumulates across polls and must be
// exactly 0x400 bytes (spec: deliberate corrections).
Result<bytes::Bytes> read_page(std::uint32_t address, IKlineFlashTransport& transport, IClock& clock,
                               const ICancellationToken& cancellation)
{
    if (auto written = write_exact(transport, ascii(std::format("rpmem 0x{:08X} 0x{:08X}", address, kPageSize)));
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    bytes::Bytes page;
    for (unsigned poll = 0; poll < kPagePolls && page.size() < kPageSize; ++poll)
    {
        auto chunk = accumulate(transport, clock, cancellation, kPageSize - page.size(), kShortTimeout);
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        page.insert(page.end(), chunk->begin(), chunk->end());
        if (page.size() > kPageSize)
        {
            return fail(ErrorKind::BadResponse, std::format("BDM page at 0x{:08X} returned {} bytes, expected {}",
                                                            address, page.size(), kPageSize));
        }
        if (auto slept = clock.sleep(kPagePollDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (page.size() != kPageSize)
    {
        return fail(ErrorKind::Timeout, std::format("BDM page at 0x{:08X} returned {} of {} bytes", address,
                                                    page.size(), kPageSize));
    }
    return page;
}

// read_mem() :82-215.
Result<bytes::Bytes> read_image(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                const ICancellationToken& cancellation, IEventSink& events)
{
    events.log(LogLevel::Info, "Reading ROM from Subaru Denso MC68HC16 with BDM");
    if (auto cleared = discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return std::unexpected(cleared.error());
    }
    const MemoryRegion region = plan.transfer_region();
    const std::uint32_t end = region.start + region.length;
    const int total_pages = static_cast<int>((region.length - (kRamEnd - kRamStart)) / kPageSize);
    int pages_done = 0;
    bytes::Bytes image;
    image.reserve(region.length);
    for (std::uint32_t address = region.start; address < end;)
    {
        if (address == kRamStart)
        {
            // read_mem() :132-144: the RAM block is never requested.
            image.insert(image.end(), kRamEnd - kRamStart, 0xff);
            address = kRamEnd;
            continue;
        }
        if (auto cancelled = cancelled_if_requested(cancellation); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        auto page = read_page(address, transport, clock, cancellation);
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        image.insert(image.end(), page->begin(), page->end());
        events.log(LogLevel::Info, std::format("BDM read addr: 0x{:08X} length: 0x{:08X}", address, kPageSize));
        events.progress(++pages_done, total_pages);
        if (auto slept = clock.sleep(kInterPageDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
        address += kPageSize;
    }
    return image;
}
} // namespace

Result<KlineConfig> SubaruDensoMc68hc16y5_02BdmExecutor::transport_setup(const FlashPlan& plan) const
{
    if (auto valid = validate_subaru_denso_mc68hc16y5_02_bdm_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& wire = std::get<SubaruDensoMc68hc16y5_02BdmPlan>(plan.family_plan());
    // execute() :50-54.
    return KlineConfig{.baud = wire.baud, .iso14230 = false, .tester_id = 0, .target_id = 0,
                       .parity = KlineParity::None};
}

Status SubaruDensoMc68hc16y5_02BdmExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                       const ICancellationToken&) const
{
    // Legacy never cleared the header; a session that left ISO-14230 framing
    // on would wrap every ASCII command (spec: deliberate corrections).
    return transport.set_add_iso14230_header(false);
}

Result<FlashExecutionResult> SubaruDensoMc68hc16y5_02BdmExecutor::execute(const FlashPlan& plan,
                                                                          IKlineFlashTransport& transport,
                                                                          IClock& clock,
                                                                          const ICancellationToken& cancellation,
                                                                          IEventSink& events)
{
    if (auto valid = validate_subaru_denso_mc68hc16y5_02_bdm_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (auto cancelled = cancelled_if_requested(cancellation); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (plan.operation() == FlashOperation::Read)
    {
        auto image = read_image(plan, transport, clock, cancellation, events);
        if (!image.has_value())
        {
            return std::unexpected(image.error());
        }
        return FlashExecutionResult{
            .operation = FlashOperation::Read, .read_bytes = std::move(*image), .rom_id = std::nullopt};
    }
    // Replaced by the kernel bootstrap in Task 3.
    return fail(ErrorKind::Unsupported, "MC68HC16Y5 BDM kernel bootstrap is not implemented");
}
} // namespace fastecu::flash
```

- [ ] **Step 4: Run to verify they pass**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_executor_test`
Expected: PASS (7 tests).

- [ ] **Step 5: Mutation-check the read corrections**

Apply each mutation alone, run the named test, confirm it FAILS, then restore with `git checkout -- src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.cpp` and confirm `git diff --exit-code` on that file:

| Mutation in `.cpp` | Test that must fail |
|---|---|
| In `read_page`, change `page.insert(page.end(), ...)` to `page.assign(chunk->begin(), chunk->end())` (legacy overwrite) | `AssemblesAPageDeliveredAcrossPolls` |
| Delete the `if (page.size() > kPageSize)` block | `OverLongPageFailsBeforeTheNextCommand` |
| Change the final `page.size() != kPageSize` to `page.empty()` (legacy non-empty acceptance) | `ShortPageFailsAfterFiftyPolls` |
| Change `set_add_iso14230_header(false)` to `Status{}` | `BeforeConfigureClearsTheIso14230Header` |

- [ ] **Step 6: Commit**

```bash
git add bazel/portable_targets.bzl src/backend/flash/ecu
git commit -m "feat(flash): read the Denso MC68HC16Y5 ROM over BDM"
```

---

### Task 3: Executor — kernel bootstrap (Write)

**Files:**
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.cpp`, `subaru_denso_mc68hc16y5_02_bdm_executor_test.cpp`

**Interfaces:**
- Consumes: Task 2's `ascii`, `cancelled_if_requested`, `write_exact`, `accumulate`, `discard`, `expect_ack`, `kShortTimeout`, `kLongTimeout`, `kExtraLongTimeout`, `kUnbounded`, `kRamStart`.
- Produces: `Status bootstrap_kernel(bytes::ByteView kernel, IKlineFlashTransport&, IClock&, const ICancellationToken&, IEventSink&)` in the anonymous namespace. `execute()` returns `FlashExecutionResult{.operation = Write, .read_bytes = nullopt, .rom_id = nullopt}` for Write.

- [ ] **Step 1: Write the failing bootstrap tests**

Append inside the anonymous namespace of `subaru_denso_mc68hc16y5_02_bdm_executor_test.cpp`:

```cpp
// 40 bytes 0x01..0x28, padded by the plan to two 32-byte chunks.
bytes::Bytes kernel_bytes()
{
    bytes::Bytes kernel;
    for (int value = 1; value <= 40; ++value)
    {
        kernel.push_back(static_cast<bytes::Byte>(value));
    }
    return kernel;
}

FlashPlan write_plan()
{
    auto plan = build_subaru_denso_mc68hc16y5_02_bdm_plan(
        FlashOperation::Write, kProtocol, kMcu, std::nullopt,
        KernelImage{.id = "bdm-kernel", .load_address = 0x20000, .bytes = kernel_bytes()});
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

enum class Gate
{
    None,
    UploadCommand, // flash_block() :394
    FirstChunk,    // flash_block() :424
    ScibCommand,   // write_mem() :268
    ScibValue,     // write_mem() :286
};

// Scripts write_mem() and flash_block(). When `broken` names a gate, that
// gate gets a same-length wrong reply and the script stops there. Returns the
// number of writes the executor must have made.
std::size_t script_bootstrap(ScriptedKlineFlashTransport& transport, Gate broken = Gate::None)
{
    const bytes::Bytes padded = *write_plan().image();
    nothing(transport); // write_mem() :226
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    if (broken == Gate::UploadCommand)
    {
        transport.queueRawRead(ascii("ACK_CMD_WPMEM"));
        return 1;
    }
    transport.queueRawRead(ascii("ACK_CMD_WDMEM"));
    for (std::size_t offset = 0; offset < padded.size(); offset += 0x20)
    {
        transport.expectRawWrite(bytes::ByteView(padded).subspan(offset, 0x20));
        if (broken == Gate::FirstChunk)
        {
            transport.queueRawRead(ascii("ACK_XX"));
            return 2;
        }
        transport.queueRawRead(ascii("ACK_WR"));
    }
    nothing(transport); // flash_block() :470
    transport.expectRawWrite(ascii("wdmem 0xFFC28 0x4"));
    if (broken == Gate::ScibCommand)
    {
        transport.queueRawRead(ascii("ACK_CMD_WPMEM"));
        return 4;
    }
    transport.queueRawRead(ascii("ACK_CMD_WDMEM"));
    nothing(transport); // write_mem() :274
    transport.expectRawWrite(bytes::Bytes{0x00, 0x0d, 0x00, 0x0c});
    if (broken == Gate::ScibValue)
    {
        transport.queueRawRead(ascii("ACK_XX"));
        return 5;
    }
    transport.queueRawRead(ascii("ACK_WR"));
    nothing(transport); // write_mem() :293
    transport.expectRawWrite(ascii("wpcsp"));
    transport.queueRawRead(ascii("??"));
    nothing(transport);
    nothing(transport);
    transport.expectRawWrite(ascii("go"));
    nothing(transport);
    return 7;
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, BootstrapsTheKernelWithTheLegacySequence)
{
    RawOnlyTransport transport;
    const std::size_t writes = script_bootstrap(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), writes);
    EXPECT_EQ(transport.framed_calls, 0);
    EXPECT_EQ(events.progress_calls.back(), (std::pair<int, int>{2, 2}));
    EXPECT_TRUE(std::ranges::any_of(events.logs, [](const auto& entry)
                                    { return entry.second == "BDM wpcsp reply: 3f 3f "; }));
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AssemblesAnAcknowledgementSplitAcrossReads)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    transport.queueRawRead(ascii("ACK_"));
    transport.queueRawRead(ascii("CMD_WDMEM"));
    const bytes::Bytes padded = *write_plan().image();
    transport.expectRawWrite(bytes::ByteView(padded).first(0x20));
    transport.queueRawRead(ascii("NAK"));
    nothing(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    // The split ACK_CMD_WDMEM passed: the executor reached the first chunk,
    // whose short "NAK" then times out.
    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, AWrongAcknowledgementAtAnyGateStopsTheUpload)
{
    for (const Gate gate : {Gate::UploadCommand, Gate::FirstChunk, Gate::ScibCommand, Gate::ScibValue})
    {
        ScriptedKlineFlashTransport transport;
        const std::size_t writes = script_bootstrap(transport, gate);
        FakeClock clock;
        FakeCancellationToken cancellation;
        RecordingEventSink events;

        auto result =
            SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

        EXPECT_THAT(result, IsErr(ErrorKind::BadResponse)) << static_cast<int>(gate);
        EXPECT_EQ(transport.writesConsumed(), writes) << static_cast<int>(gate);
        EXPECT_TRUE(transport.scriptConsumed()) << static_cast<int>(gate);
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, ASilentBridgeTimesOut)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    nothing(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationBetweenChunksStopsBeforeTheNextChunk)
{
    ScriptedKlineFlashTransport transport;
    nothing(transport);
    transport.expectRawWrite(ascii("wdmem 0x00020000 0x00000040"));
    transport.queueRawRead(ascii("ACK_CMD_WDMEM"));
    transport.expectRawWrite(bytes::ByteView(*write_plan().image()).first(0x20));
    transport.queueRawRead(ascii("ACK_WR"));
    FakeClock clock;
    FakeCancellationToken cancellation;
    // Trips once the buffer clear, the upload ACK and the first chunk ACK are read.
    cancellation.set_predicate([&transport] { return transport.read_timeouts_.size() >= 3; });
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruDensoMc68hc16y5_02BdmExecutor, CancellationAfterGoStillSucceeds)
{
    ScriptedKlineFlashTransport transport;
    script_bootstrap(transport);
    FakeClock clock;
    FakeCancellationToken cancellation;
    // Trips as soon as `go` has been written: the kernel is already running.
    cancellation.set_predicate([&transport] { return transport.writesConsumed() >= 7; });
    RecordingEventSink events;

    auto result = SubaruDensoMc68hc16y5_02BdmExecutor{}.execute(write_plan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(transport.writesConsumed(), 7U);
}
```

In `CancellationAfterGoStillSucceeds`, the scripted post-`go` empty read stays unconsumed, because the cancelled `accumulate` returns before reading. That is why this test does not assert `scriptConsumed()`.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_executor_test`
Expected: the six new tests FAIL with `Unsupported` from the Task 2 placeholder. The Task 2 tests still PASS.

- [ ] **Step 3: Implement the bootstrap**

In the anonymous namespace of `subaru_denso_mc68hc16y5_02_bdm_executor.cpp`, after `read_image`, add:

```cpp
constexpr std::size_t kUploadChunk = 0x20;
constexpr std::string_view kAckCommand = "ACK_CMD_WDMEM";
constexpr std::string_view kAckWrite = "ACK_WR";

Status send_and_ack(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                    bytes::ByteView request, std::string_view token, std::chrono::milliseconds budget)
{
    if (auto written = write_exact(transport, request); !written.has_value())
    {
        return written;
    }
    return expect_ack(transport, clock, cancellation, token, budget);
}

// write_mem() :296-305 reads and logs these replies without checking them;
// the bridge's replies are unknown, so they stay ungated (spec appendix).
Status log_reply(std::string_view command, IKlineFlashTransport& transport, IClock& clock,
                 const ICancellationToken& cancellation, IEventSink& events)
{
    auto reply = accumulate(transport, clock, cancellation, kUnbounded, kLongTimeout);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    events.log(LogLevel::Info, std::format("BDM {} reply: {}", command, bytes::toHex(*reply)));
    return {};
}

// write_mem() :217-350 and flash_block() :352-474. Uploads the padded kernel to
// RAM, enables SCIB, sets PC/SP and starts the kernel. The ROM is not written.
Status bootstrap_kernel(bytes::ByteView kernel, IKlineFlashTransport& transport, IClock& clock,
                        const ICancellationToken& cancellation, IEventSink& events)
{
    events.log(LogLevel::Info, "Uploading kernel to Subaru Denso MC68HC16 RAM with BDM");
    // write_mem() :226.
    if (auto cleared = discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return cleared;
    }
    // flash_block() :378-399.
    events.log(LogLevel::Info, std::format("Writing block: 00 at address 0x{:08X}", kRamStart));
    if (auto started = send_and_ack(transport, clock, cancellation,
                                    ascii(std::format("wdmem 0x{:08X} 0x{:08X}", kRamStart, kernel.size())),
                                    kAckCommand, kExtraLongTimeout);
        !started.has_value())
    {
        return started;
    }
    // flash_block() :402-468.
    const std::size_t chunks = kernel.size() / kUploadChunk;
    for (std::size_t index = 0; index < chunks; ++index)
    {
        if (auto cancelled = cancelled_if_requested(cancellation); !cancelled.has_value())
        {
            return cancelled;
        }
        if (auto acked = send_and_ack(transport, clock, cancellation, kernel.subspan(index * kUploadChunk, kUploadChunk),
                                      kAckWrite, kLongTimeout);
            !acked.has_value())
        {
            return acked;
        }
        events.progress(static_cast<int>(index + 1), static_cast<int>(chunks));
    }
    events.log(LogLevel::Info, "Block write complete.");
    // flash_block() :470.
    if (auto cleared = discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return cleared;
    }
    // write_mem() :258-275: enable SCIB (the kernel sets 62500 baud itself).
    if (auto scib = send_and_ack(transport, clock, cancellation, ascii("wdmem 0xFFC28 0x4"), kAckCommand,
                                 kExtraLongTimeout);
        !scib.has_value())
    {
        return scib;
    }
    if (auto cleared = discard(transport, clock, cancellation, events, kLongTimeout); !cleared.has_value())
    {
        return cleared;
    }
    // write_mem() :277-294.
    if (auto scib = send_and_ack(transport, clock, cancellation, bytes::Bytes{0x00, 0x0d, 0x00, 0x0c}, kAckWrite,
                                 kLongTimeout);
        !scib.has_value())
    {
        return scib;
    }
    if (auto cleared = discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return cleared;
    }
    // write_mem() :296-305.
    if (auto written = write_exact(transport, ascii("wpcsp")); !written.has_value())
    {
        return written;
    }
    for (int reply = 0; reply < 2; ++reply)
    {
        if (auto logged = log_reply("wpcsp", transport, clock, cancellation, events); !logged.has_value())
        {
            return logged;
        }
    }
    // write_mem() :317-323. Once `go` is sent the kernel is running: nothing
    // after it can fail or cancel the operation.
    if (auto cancelled = cancelled_if_requested(cancellation); !cancelled.has_value())
    {
        return cancelled;
    }
    if (auto written = write_exact(transport, ascii("go")); !written.has_value())
    {
        return written;
    }
    if (auto reply = accumulate(transport, clock, cancellation, kUnbounded, kLongTimeout); reply.has_value())
    {
        events.log(LogLevel::Info, std::format("BDM go reply: {}", bytes::toHex(*reply)));
    }
    else
    {
        events.log(LogLevel::Warning, std::format("BDM go reply not read: {}", reply.error().detail));
    }
    return {};
}
```

Then, in `execute()`, replace the two placeholder lines (`// Replaced by the kernel bootstrap in Task 3.` and the `return fail(ErrorKind::Unsupported, ...)`) with:

```cpp
    if (auto booted = bootstrap_kernel(*plan.image(), transport, clock, cancellation, events); !booted.has_value())
    {
        return std::unexpected(booted.error());
    }
    return FlashExecutionResult{.operation = FlashOperation::Write, .read_bytes = std::nullopt, .rom_id = std::nullopt};
```

- [ ] **Step 4: Run to verify they pass**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_executor_test`
Expected: PASS (13 tests).

- [ ] **Step 5: Mutation-check the gating**

Apply each mutation alone, run the named test, confirm it FAILS, then restore and confirm `git diff --exit-code` on the file:

| Mutation in `.cpp` | Test that must fail |
|---|---|
| In `expect_ack`, replace the final `return fail(ErrorKind::BadResponse, ...)` with `return {};` | `AWrongAcknowledgementAtAnyGateStopsTheUpload` |
| In `bootstrap_kernel`, delete the chunk-loop `cancelled_if_requested` check | `CancellationBetweenChunksStopsBeforeTheNextChunk` |
| In `bootstrap_kernel`, change the post-`go` `else` branch to `return std::unexpected(reply.error());` | `CancellationAfterGoStillSucceeds` |

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu
git commit -m "feat(flash): bootstrap the Denso MC68HC16Y5 kernel over BDM"
```

---

### Task 4: Desktop workflow, dispatch cut-over, legacy deletion

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.h:35-43`, `flash_workflow.cpp`, `flash_workflow_test.cpp`, `src/platform/desktop/common/flash/BUILD.bazel:71-72`
- Modify: `src/ui/desktop/flash/common/flash_dialog.cpp:185`, `src/ui/desktop/mainwindow.cpp:1247-1254`, `src/ui/desktop/mainwindow.h:60`, `src/ui/desktop/BUILD.bazel:107`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel:11-36`, `scripts/check-legacy-flash-drain.py:34`
- Delete: `src/ui/desktop/flash/bdm/` (3 files), `src/platform/desktop/common/flash/legacy/bdm/` (2 files)

**Interfaces:**
- Consumes: `build_subaru_denso_mc68hc16y5_02_bdm_plan`, `SubaruDensoMc68hc16y5_02BdmExecutor`, `resolveKernel(request, repository)`, `QtFileRepository`, `DesktopKlineFlashTransport`, `QtClock`, `FlashAttemptOutcome`, `bind_flash_attempt`.
- Produces: `FlashPromptKind::ConfirmBdmKernelBootstrap`, `Route::Kind::SubaruDensoMc68hc16y5_02Bdm`, `class SubaruDensoMc68hc16y5_02BdmWorkflow`.

- [ ] **Step 1: Write the failing workflow tests**

In `src/platform/desktop/common/flash/flash_workflow_test.cpp`:

Add this entry to `catalogPaths()`'s `<protocols>`, after the `sub_ecu_denso_mc68hc16y5_02_tpu` entry. It reuses the existing `catalog_mc68.bin` file (`11 22 33`):

```xml
    <protocol name="sub_ecu_denso_mc68hc16y5_02_bdm">
      <ecu>Denso MC68HC16Y5</ecu><mcu>MC68HC16Y5</mcu>
      <kernel>catalog_mc68.bin</kernel><kernel_addr>0x20000</kernel_addr>
    </protocol>
```

Add `"sub_ecu_denso_mc68hc16y5_02_bdm",` to the `portable` list in `recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone()`.

Rename the private slot `mc68BdmProtocolIsNotClaimedByPortableRoute` to `mc68BdmReadRoutesThroughBeginToAttempt`, and add four more slot declarations next to it:

```cpp
    void mc68BdmReadRoutesThroughBeginToAttempt();
    void mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom();
    void mc68BdmDeclinedBootstrapConfirmationCancels();
    void mc68BdmTestWriteFailsBeforeAnyPrompt();
    void mc68BdmPrefixLookalikeStaysOffTheKlineFamily();
```

Replace the body of the old `mc68BdmProtocolIsNotClaimedByPortableRoute()` with these five definitions:

```cpp
void FlashWorkflowTest::mc68BdmReadRoutesThroughBeginToAttempt()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    QCOMPARE(plan.family(), FlashFamily::SubaruDensoMc68hc16y5_02Bdm);
    QCOMPARE(plan.transport(), TransportKind::Kline);
    QCOMPARE(plan.transfer_region(), (MemoryRegion{0, 0x30000}));
    QVERIFY(!plan.image().has_value());
}

void FlashWorkflowTest::mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        QFAIL(failure->error.detail.c_str());
    }
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ConfirmBdmKernelBootstrap);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    bytes::Bytes expected(0x20, 0x00);
    expected[0] = 0x11;
    expected[1] = 0x22;
    expected[2] = 0x33;
    QCOMPARE(plan.image(), std::optional<bytes::Bytes>(expected));
    QCOMPARE(plan.transfer_region(), (MemoryRegion{0x20000, 0x20}));
    QVERIFY(!plan.kernel().has_value());
}

void FlashWorkflowTest::mc68BdmDeclinedBootstrapConfirmationCancels()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ConfirmBdmKernelBootstrap);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::mc68BdmTestWriteFailsBeforeAnyPrompt()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::TestWrite);
    input.mcu = "MC68HC16Y5";
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

void FlashWorkflowTest::mc68BdmPrefixLookalikeStaysOffTheKlineFamily()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm_x");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}
```

If `QCOMPARE` on `MemoryRegion` or `std::optional<bytes::Bytes>` does not compile for lack of a `QTest::toString`, use `QVERIFY(a == b)` for those comparisons instead.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow`
Expected: compile FAIL on `FlashPromptKind::ConfirmBdmKernelBootstrap` and `FlashFamily::SubaruDensoMc68hc16y5_02Bdm` usage via the workflow.

- [ ] **Step 3: Implement the workflow**

In `src/platform/desktop/common/flash/flash_workflow.h`, add `ConfirmBdmKernelBootstrap,` to `enum class FlashPromptKind` after `ConfirmSh7058Read,`.

In `src/platform/desktop/common/flash/flash_workflow.cpp`:
- add includes next to the Unisia Jecs ones:
  ```cpp
  #include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"
  #include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
  ```
- add this class after `SubaruUnisiaJecsWorkflow`:

```cpp
class SubaruDensoMc68hc16y5_02BdmWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruDensoMc68hc16y5_02BdmWorkflow(FlashWorkflowRequest request) : request_(std::move(request))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!plan_.has_value())
        {
            plan_ = buildPlan();
        }
        if (!plan_->has_value())
        {
            return FlashFailureStep{plan_->error()};
        }
        if (outcome_.hasFailure())
        {
            return outcome_.takeFailure();
        }
        if (outcome_.terminal())
        {
            return outcome_.completedStep();
        }
        if (!begun_)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (!attempted_)
        {
            if ((*plan_)->operation() == FlashOperation::Write && !bootstrap_confirmed_)
            {
                return FlashPromptStep{FlashPromptKind::ConfirmBdmKernelBootstrap, {}};
            }
            attempted_ = true;
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     bind_flash_attempt(std::move(**plan_),
                                                        std::make_unique<SubaruDensoMc68hc16y5_02BdmExecutor>(),
                                                        std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                     std::make_unique<QtClock>()};
        }
        return outcome_.completedStep();
    }

    void submit(FlashPromptResponse response) override
    {
        if (response != FlashPromptResponse::Accept)
        {
            outcome_.cancel();
            return;
        }
        if (!begun_)
        {
            begun_ = true;
        }
        else
        {
            bootstrap_confirmed_ = true;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        outcome_.record(std::move(result));
    }

  private:
    // The operator's ROM (request_.image) is never forwarded: Write uploads
    // and starts the cfg kernel; it does not write the ROM.
    Result<FlashPlan> buildPlan()
    {
        if (request_.operation != FlashOperation::Write)
        {
            return build_subaru_denso_mc68hc16y5_02_bdm_plan(request_.operation, request_.protocol, request_.mcu,
                                                            std::nullopt, std::nullopt);
        }
        QtFileRepository repository;
        Result<KernelImage> kernel = resolveKernel(request_, repository);
        if (!kernel.has_value())
        {
            return std::unexpected(kernel.error());
        }
        return build_subaru_denso_mc68hc16y5_02_bdm_plan(request_.operation, request_.protocol, request_.mcu,
                                                        std::nullopt, std::move(*kernel));
    }

    FlashWorkflowRequest request_;
    std::optional<Result<FlashPlan>> plan_;
    bool begun_ = false;
    bool bootstrap_confirmed_ = false;
    bool attempted_ = false;
    FlashAttemptOutcome outcome_;
};
```

- add `SubaruDensoMc68hc16y5_02Bdm,` to `Route::Kind` before `Unrouted`;
- replace the route row and its comment:
  ```cpp
      // Reserve this longer prefix before the bare MC68 _02 row. BDM remains
      // on its legacy path and must not be swallowed by portable routing.
      {"sub_ecu_denso_mc68hc16y5_02_bdm", Unrouted},
  ```
  with:
  ```cpp
      // Keep this longer prefix before the bare MC68 _02 row so no _02_bdm*
      // name reaches the K-Line family; the BDM plan rejects all but the exact
      // protocol.
      {"sub_ecu_denso_mc68hc16y5_02_bdm", SubaruDensoMc68hc16y5_02Bdm},
  ```
- add to the factory switch, before `case Unrouted:`:
  ```cpp
      case SubaruDensoMc68hc16y5_02Bdm:
          return std::make_unique<SubaruDensoMc68hc16y5_02BdmWorkflow>(std::move(request));
  ```

In `src/platform/desktop/common/flash/BUILD.bazel`, add to the `flash_workflow` target's deps, next to the Unisia Jecs entries:
```starlark
        "//src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_executor",
        "//src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_bdm_plan",
```

In `src/ui/desktop/flash/common/flash_dialog.cpp`, before the `if (prompt.kind == FlashPromptKind::ConfirmSh7058Read)` block, add:

```cpp
    if (prompt.kind == FlashPromptKind::ConfirmBdmKernelBootstrap)
    {
        return QMessageBox::warning(this, tr("BDM kernel bootstrap"),
                                    tr("This uploads the flash kernel into ECU RAM through the BDM adapter and starts "
                                       "it. It does not write the ROM.\n\nContinue?"),
                                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes
                   ? FlashPromptResponse::Accept
                   : FlashPromptResponse::Decline;
    }
```

- [ ] **Step 4: Run to verify they pass**

Run: `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow //src/ui/desktop/flash/common:all`
Expected: PASS, including every pre-existing slot.

- [ ] **Step 5: Cut over MainWindow and delete the legacy path**

- In `src/ui/desktop/mainwindow.cpp`, delete the block:
  ```cpp
          /*
           * Denso ECU BDM
           */
          else if (configValues->flash_protocol_selected_protocol_name.startsWith("sub_ecu_denso_mc68hc16y5_02_bdm"))
          {
              FlashEcuSubaruDensoMC68HC16Y5_02_BDM flash_module(serial, ecuCalDef[rom_number], cmd_type, this);
              connect_signals_and_run_module(&flash_module);
          }
  ```
  The `else if` for `sub_ecu_unisia_jecs_20_bootmode` that follows now continues the chain directly.
- In `src/ui/desktop/mainwindow.h`, delete `#include "src/ui/desktop/flash/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm.h"`.
- In `src/ui/desktop/BUILD.bazel`, delete `"//src/ui/desktop/flash/bdm",`.
- In `src/platform/desktop/common/flash/legacy/BUILD.bazel`:
  - delete `"bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.cpp",` from `srcs` and `"bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.h",` from `hdrs`;
  - in the comment above `hdrs`, change `the bdm/bootmode/ecu/` + `jtag legacy Qt operation classes` to `the bootmode/ecu/jtag legacy Qt operation classes`, and `Every *_operation.h in the four family subdirectories` to `Every *_operation.h in the three family subdirectories`. Append `The bdm/ subdirectory is gone: wave 6c-1 drained it.` after the tcu sentence;
  - change `Every *.h in the five directories (root + four families)` to `Every *.h in the four directories (root + three families)`.
- Delete the files:
  ```bash
  git rm -r src/ui/desktop/flash/bdm src/platform/desktop/common/flash/legacy/bdm
  ```
- In `scripts/check-legacy-flash-drain.py`, remove the line `"bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.cpp",` from `REMAINING`.
- Confirm nothing else references the class: `grep -rn "FlashEcuSubaruDensoMC68HC16Y5_02_BDM\|flash/bdm\|legacy/bdm" src apps tests bazel BUILD.bazel`. Expect no matches.

- [ ] **Step 6: Build and run the affected suites**

```bash
bazel build --config=release //:fastecu
bazel test --config=release //src/platform/desktop/common/flash/... //src/backend/flash/... //src/ui/desktop/...
python3 scripts/check-legacy-flash-drain.py   # expect "OK: 3 families remaining, none added."
```
Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add -A src/platform/desktop/common/flash src/ui/desktop scripts/check-legacy-flash-drain.py
git commit -m "feat(flash): route Denso MC68HC16Y5 BDM through the portable workflow"
```

---

### Task 5: Docs, gates, PR

**Files:**
- Modify: `docs/flash-qualification-matrix.md:61`, `docs/modularization-plan.md:73-76`, `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md` (append), `docs/superpowers/specs/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm-design.md` (status + checklist link)
- Create: `docs/denso-mc68hc16-bdm-bench-checklist.md`

- [ ] **Step 1: Flip the matrix row**

Replace the `FlashEcuSubaruDensoMC68HC16Y5_02_BDM` row with (one line):

```markdown
| FlashEcuSubaruDensoMC68HC16Y5_02_BDM | BDM | BDM | read, write (kernel bootstrap) | yes | `subaru_denso_mc68hc16y5_02_bdm_plan_test`, `subaru_denso_mc68hc16y5_02_bdm_executor_test`, `test_flash_workflow` @ Wave 6c-1 | experimental | — | Exact protocol/MCU pair `sub_ecu_denso_mc68hc16y5_02_bdm` / `MC68HC16Y5`; test_write is rejected. The BDM bridge is driven with its ASCII command protocol at 115200 baud through `write_raw()` / `read_raw()` only. Read returns the physical `0x0–0x2FFFF` image with the `0x20000–0x27FFF` RAM hole filled with `0xFF`, as legacy did. **Write is a kernel bootstrap, not a ROM write:** it uploads the cfg kernel into RAM at `0x20000`, enables SCIB, sets PC/SP and sends `go`; an extra confirmation says so before any I/O. **Deliberate corrections:** replies no longer pass through the K-Line frame parser, which could not carry ASCII acknowledgements or 1 KiB pages on direct serial; pages accumulate across polls and must be exactly 0x400 bytes; every `ACK_CMD_WDMEM` / `ACK_WR` is gated; the operator's ROM buffer is no longer overwritten with the kernel; a stale ISO-14230 header is cleared before configuration. The `wpcsp` / `go` replies remain unchecked and commands still carry no terminator; see the [bench checklist](denso-mc68hc16-bdm-bench-checklist.md). No adapter guard: selecting this protocol with an OpenPort 2.0 reads replies through J2534. No hardware qualification is claimed. |
```

- [ ] **Step 2: Write the bench checklist**

`docs/denso-mc68hc16-bdm-bench-checklist.md`:

```markdown
# Subaru Denso MC68HC16Y5 BDM bench checklist

## 0. STOP — not hardware-qualified

Do not treat this family as proven until every section below has passed on
real hardware. Record the BDM bridge (make, model, firmware), the serial
adapter and port, the ECU part number and identifier, date, operator and
result.

## 1. Bridge protocol

- Capture a serial trace of one read and one kernel bootstrap.
- Confirm the bridge accepts commands with no line terminator
  (`rpmem 0x00000000 0x00000400`). If it needs CR or LF, stop and record it.
- Record the exact bytes the bridge sends after `wpcsp` and after `go`. A
  later change will gate them; until then they are only logged.

## 2. Read

- Read the full ROM and compare `0x00000–0x1FFFF` and `0x28000–0x2FFFF`
  byte-for-byte with a trusted K-Line dump of the same ECU.
- Confirm `0x20000–0x27FFF` is `0xFF` in the saved image.
- Pull the bridge cable mid-read and confirm the operation fails instead of
  returning a short image.

## 3. Kernel bootstrap

- Run Write with the stock `ssmk_mc68hc916y5.bin` kernel. Confirm every chunk
  is acknowledged with `ACK_WR` and the operation ends after `go`.
- Confirm the loaded ROM in FastECU is unchanged afterwards.
- Confirm the kernel is running: connect with the
  `sub_ecu_denso_mc68hc16y5_02` K-Line protocol and read the ROM.
```

- [ ] **Step 3: Update the specs and the modularization plan**

In `docs/superpowers/specs/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm-design.md`:
- change the status line to `**Status:** design approved; implementation plan in [the 6c-1 plan](../plans/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm.md).`
- replace ``- New BDM bench checklist, `docs/denso-mc68hc16-bdm-bench-checklist.md`
  (linked once it exists), gating both operations.`` with `- New [BDM bench checklist](../../denso-mc68hc16-bdm-bench-checklist.md) gating both operations.`

Append to `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md`:

```markdown
## Wave 6c-1 implementation note

The [Denso MC68HC16Y5 BDM family spec](2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm-design.md)
records three departures from this design: the 6a-3/6a-4/6b-2
behavior-correction exception applies, because legacy's framed
`read_serial_data()` could not carry the bridge's ASCII replies on direct
serial; Write is modeled as a kernel bootstrap whose plan image is the padded
cfg kernel; and 6c-1 adds no port method, because `write_raw()` / `read_raw()`
(port item 3) landed early in #351. The drain moves from four entries to three.
```

In `docs/modularization-plan.md`, replace the four-line `- Wave 6a-3 ...` bullet with:

```markdown
- Wave 6a-3 `FlashEcuSubaruHitachiSH72543rCan` — merged (#349). See the
  [family design](superpowers/specs/2026-09-20-step5-tail-wave6a3-hitachi-sh72543r-can-design.md).
- Wave 6a-4 `FlashEcuSubaruHitachiSH7058Can` — merged (#350).
- Wave 6b-1 `FlashEcuSubaruUnisiaJecs` — merged (#352), on the raw K-Line
  transport foundation (#351).
- Wave 6b-2 `FlashEcuSubaruDensoSH705xKline` — merged (#355, #356).
- Wave 6c-1 `FlashEcuSubaruDensoMC68HC16Y5_02_BDM` — implemented on this
  branch. The drain is three remaining families. Hardware status remains
  experimental. See the
  [family design](superpowers/specs/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm-design.md).
```

- [ ] **Step 4: Run all gates**

```bash
bazel test --config=release //...
bazel build --config=release //:fastecu //:portable_closure
prek run --all-files
bazel run //:clang_tidy_report_changed
python3 scripts/check-legacy-flash-drain.py
scripts/coverage-local.sh   # confirm >=80% on the new executor/plan sources
```
Expected: all green; 3 families remaining; clang-tidy reports no findings in changed files. Fix coverage gaps with tests, not exclusions. If `//tests:serial_backend_tests` fails only on Windows CI, it is a known pre-existing flake: rerun it rather than debugging this branch.

- [ ] **Step 5: Commit**

```bash
git add docs
git commit -m "docs(flash): record the Denso MC68HC16Y5 BDM migration"
```

- [ ] **Step 6: Ask the user for approval to push, then open the PR**

After approval:
```bash
git push -u origin feat/wave6c1-denso-mc68hc16-bdm
gh pr create --title "feat(flash): migrate Subaru Denso MC68HC16Y5 BDM (wave 6c-1)" --body "$(cat <<'EOF'
### What
- portable plan and executor for `sub_ecu_denso_mc68hc16y5_02_bdm`: ROM read and kernel bootstrap over the BDM bridge's ASCII protocol, through `write_raw()` / `read_raw()` only
- Write is modeled as what it is — upload the cfg kernel to RAM and start it — with an extra confirmation before any I/O; the operator's ROM is never touched
- route through the portable desktop workflow; remove the legacy dialog, operation and MainWindow branch
- drain ratchet four -> three; matrix row `experimental`; bench checklist

### Why
- continue the step 5 legacy flash drain (wave 6c-1)
- legacy read replies through the K-Line frame parser, which cannot carry the bridge's ASCII acknowledgements or 1 KiB pages on direct serial
- corrections under the 6a-3/6a-4/6b-2 policy: accumulated exact-size pages, gated ACKs, no kernel written over the ROM buffer, stale ISO-14230 header cleared

### Verification
- bazel test --config=release //...
- bazel build --config=release //:fastecu //:portable_closure
- prek run --all-files; bazel run //:clang_tidy_report_changed
- mutation checks for every correction

### References
- design: docs/superpowers/specs/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm-design.md
- plan: docs/superpowers/plans/2026-09-25-step5-tail-wave6c1-denso-mc68hc16-bdm.md
- hardware qualification blocked by docs/denso-mc68hc16-bdm-bench-checklist.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01BVWAZwWDdZGx6y3Ks4ptJX
EOF
)"
```
