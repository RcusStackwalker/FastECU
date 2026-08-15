# Step 5 Tail Wave 3 — Hitachi/Mitsu M32R CAN Cluster Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port four legacy Qt flash operation classes — `FlashEcuSubaruHitachiM32rCan`, `FlashTcuCvtSubaruHitachiM32rCan`, `FlashTcuCvtSubaruMitsuMH8111Can`, `FlashTcuCvtSubaruMitsuMH8104Can` — to portable `FlashPlan` + `IFlashExecutor` pairs, following the wave-0/1/2 template, and shrink `//:legacy_flash_drain`'s `REMAINING` set by four entries.

**Architecture:** Each family gets a leaf `<family>_types.h` (plan struct), `<family>_plan.{h,cpp}` (builder + validator, no I/O), and `<family>_executor.{h,cpp}` (an `IFlashExecutor` driving the family's wire sequence over `UdsClient`/`CanFlashUdsChannel`, exactly as `MitsuColtM32rCanExecutor` does). All four are kernel-free, CAN/ISO-15765, and reuse the existing UDS layer with no new port surface. `flash_workflow.cpp` gains one parametrized workflow class serving all four (no per-family confirmations, no kernel resolution — the simplest workflow shape in the file). One cluster-factoring PR closes the wave for the MH8111/MH8104 pair.

**Tech Stack:** C++23, Bazel, GoogleTest/GoogleMock, existing `fastecu::flash` backend (`src/backend/flash/`), existing `uds::UdsClient`/`uds::IUdsChannel` (`src/backend/protocol/uds/`).

**Spec:** [docs/superpowers/specs/2026-08-15-step5-tail-wave3-hitachi-mitsu-m32r-can-design.md](../specs/2026-08-15-step5-tail-wave3-hitachi-mitsu-m32r-can-design.md)

## Global Constraints

- **CAN configuration.** `bitrate = 500000`, `extended_id = false` for all four families. `FlashEcuSubaruHitachiM32rCan` uses `request_id = 0x7e0`, `response_id = 0x7e8`. The three TCU families use `request_id = 0x7e1`, `response_id = 0x7e9`.
- **Envelope.** Every request/response goes through `CanFlashUdsChannel`, exactly as `mitsu_colt_m32r_can_executor.cpp` uses it — PDUs in executor code start at the SID; the 4-byte `00 00 07 <id>` envelope is added/stripped by the channel, never hand-built in the executor.
- **Crypto.** All four families call the already-portable `SsmProtocol::calculateSeedKey` / `SsmProtocol::calculatePayload` (declared in `src/algorithms/protocol/ssm/ssm_protocol.h`) with the shared 32-byte `indextransformation` table `{0x5,0x6,0x7,0x1,0x9,0xC,0xD,0x8,0xA,0xD,0x2,0xB,0xF,0x4,0x0,0x3,0xB,0x4,0x6,0x0,0xF,0x2,0xD,0x9,0x5,0xC,0x1,0xA,0x3,0xD,0xE,0x8}` in every case. Only each family's `keytogenerateindex` (16 or 4 entries) differs — reproduced verbatim per family below. No new crypto code is written; only new constant tables.
- **cfg identifiers** (all `mode=OBD2`, `flash_transport=iso15765`, `test_write=no`, verified against `resources/shared/config/protocols.cfg`):

  | Protocol | mcu_type | FlashFamily |
  |---|---|---|
  | `sub_ecu_hitachi_m32r_can` | `M32R_512KB_1block` | `SubaruHitachiM32rCan` |
  | `sub_tcu_cvt_hitachi_m32r_can` | `M32R_512KB` | `SubaruTcuCvtHitachiM32rCan` |
  | `sub_tcu_cvt_mitsu_mh8111_can` | `MH8111` | `SubaruTcuCvtMitsuMh8111Can` |
  | `sub_tcu_cvt_mitsu_mh8104_can` | `MH8104` | `SubaruTcuCvtMitsuMh8104Can` |

- **Flash device geometry**, read from `src/backend/definitions/kernelmemorymodels.h` (`flashdevices[]`) — every plan validator must cross-check these exactly, following `subaru_hitachi_m32r_kline_plan.cpp`'s `validate_identity` precedent (asserting `romsize`/`numblocks`/`fblocks[i]` match, not just trusting the mcu string):

  | mcu_type | romsize | numblocks | fblocks (start, len) |
  |---|---|---|---|
  | `M32R_512KB_1block` | `0x80000` | 1 | `{0x00000, 0x80000}` |
  | `M32R_512KB` | `0x80000` | 11 | `{0,0x4000},{0x4000,0x2000},{0x6000,0x2000},{0x8000,0x10000},{0x10000,0x10000},{0x20000,0x10000},{0x30000,0x10000},{0x40000,0x10000},{0x50000,0x10000},{0x60000,0x10000},{0x70000,0x10000}` |
  | `MH8111` | `0x180000` | 4 | `{0,0x40000},{0x40000,0x20000},{0x60000,0x20000},{0x80000,0x100000}` |
  | `MH8104` | `0x80000` | 4 | `{0,0x4000},{0x4000,0x2000},{0x6000,0x2000},{0x8000,0x78000}` |

- **`block_modified` masks are baked into the plan, not user-selectable.** Legacy `write_mem` iterates `for (blockno = 0; blockno < flashdevices[...].numblocks; blockno++)`, so only the first `numblocks` entries of the (oversized, 16-entry) `block_modified` array matter:
  - `SubaruHitachiM32rCan` (`numblocks=1`): block 0 only → the entire `{0x00000, 0x80000}` region is one `reflash_block` call.
  - `SubaruTcuCvtHitachiM32rCan` (`numblocks=11`): blocks 0-2 skipped, blocks 3-10 flashed → contiguous `{0x8000, 0x78000}`, eight `reflash_block` calls of `0x10000` bytes each.
  - `SubaruTcuCvtMitsuMh8111Can` (`numblocks=4`): blocks 0-2 skipped, block 3 flashed → `{0x80000, 0x100000}`, one `reflash_block` call. **This does not overlap the read window (see below) — a genuine legacy asymmetry, preserved, not fixed.**
  - `SubaruTcuCvtMitsuMh8104Can` (`numblocks=4`): blocks 0-2 skipped, block 3 flashed → `{0x8000, 0x78000}`, one `reflash_block` call.
- **Read windows are hardcoded in legacy, not derived from the caller's `start_addr`/`length` arguments** (each `read_mem` overwrites its parameters immediately — "hack for testing" in the legacy comments):
  - `SubaruHitachiM32rCan`: `{0x00000, 0x80000}` (full ROM).
  - `SubaruTcuCvtHitachiM32rCan`: legacy computes `start_addr = start_addr - 0x00100000` where the caller always passes `start_addr = 0`, which **underflows** `uint32_t` to `0xFFF00000` and bypasses the `if (start_addr < 0x8000)` floor-clamp entirely (0xFFF00000 is not less than 0x8000) — this code path never ran in production (see Task 3), so there is no observed hardware behavior to preserve. **This plan targets the clamp's evident intent** — `{0x8000, 0x78000}`, matching every sibling family's read window — documented as the wave's second deliberate divergence (see Task 3).
  - `SubaruTcuCvtMitsuMh8111Can`: `{0x8000, 0x78000}` (hardcoded directly, no bias arithmetic).
  - `SubaruTcuCvtMitsuMh8104Can`: `{0x8000, 0x78000}` (hardcoded directly, no bias arithmetic).
- **Read padding.** After decrypting the dumped bytes, `SubaruTcuCvtHitachiM32rCan` prepends `0x8000` bytes of `0x00`; `SubaruTcuCvtMitsuMh8111Can` and `SubaruTcuCvtMitsuMh8104Can` prepend `0x8000` bytes of `0xFF`. `SubaruHitachiM32rCan` has no padding (its read window starts at 0). Preserve every value exactly — this is a real per-family divergence, not a copy-paste artifact.
- **Chunk sizes.** Every family's kernel-dump read (`0xB7`) uses a fixed `0x100` (256-byte) page. Write (`0xB6`, in `reflash_block`) chunk size varies: `SubaruHitachiM32rCan` 256 B, `SubaruTcuCvtHitachiM32rCan` 128 B, `SubaruTcuCvtMitsuMh8111Can` 256 B, `SubaruTcuCvtMitsuMh8104Can` 128 B.
- **Error taxonomy.** Every executor test file covers, per the umbrella: success path, timeout (empty scripted read), disconnect (`queue_error(ErrorKind::Disconnected, ...)`), negative/malformed response, cancellation before I/O and mid-transfer, and (via the plan test) unsupported/invalid-config rejection before any I/O.
- **Confirmations.** None of the four families declare a `ConfirmationSpec` — none carries a documented high-risk step comparable to Colt's "caused bootloader lockup" comment. `fields.confirmations` stays empty in every builder.
- **`family_requires_kernel_v` is `false`** for all four new `FamilyPlan` alternatives — each family jumps to the ECU/TCU's own on-board kernel via SecurityAccess + `0x10`/`0x42`, uploading no image.

## File structure

```
src/backend/flash/ecu/
  subaru_hitachi_m32r_can_types.h              (new, Task 1)
  subaru_hitachi_m32r_can_plan.{h,cpp}         (new, Task 1)
  subaru_hitachi_m32r_can_plan_test.cpp        (new, Task 1)
  subaru_hitachi_m32r_can_executor.{h,cpp}     (new, Task 1)
  subaru_hitachi_m32r_can_executor_test.cpp    (new, Task 1)

  subaru_tcu_cvt_hitachi_m32r_can_types.h              (new, Task 3)
  subaru_tcu_cvt_hitachi_m32r_can_plan.{h,cpp}         (new, Task 3)
  subaru_tcu_cvt_hitachi_m32r_can_plan_test.cpp        (new, Task 3)
  subaru_tcu_cvt_hitachi_m32r_can_executor.{h,cpp}     (new, Task 3)
  subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp    (new, Task 3)

  subaru_tcu_cvt_mitsu_mh8111_can_types.h              (new, Task 4)
  subaru_tcu_cvt_mitsu_mh8111_can_plan.{h,cpp}         (new, Task 4)
  subaru_tcu_cvt_mitsu_mh8111_can_plan_test.cpp        (new, Task 4)
  subaru_tcu_cvt_mitsu_mh8111_can_executor.{h,cpp}     (new, Task 4)
  subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp    (new, Task 4)

  subaru_tcu_cvt_mitsu_mh8104_can_types.h              (new, Task 5)
  subaru_tcu_cvt_mitsu_mh8104_can_plan.{h,cpp}         (new, Task 5)
  subaru_tcu_cvt_mitsu_mh8104_can_plan_test.cpp        (new, Task 5)
  subaru_tcu_cvt_mitsu_mh8104_can_executor.{h,cpp}     (new, Task 5)
  subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp    (new, Task 5)

  subaru_tcu_cvt_mitsu_can_common.{h,cpp}              (new, Task 6 — MH8111/MH8104 factoring)
  subaru_tcu_cvt_mitsu_can_common_test.cpp             (new, Task 6)

  BUILD.bazel                                          (modified, every task)

src/backend/flash/
  flash_types.h                                        (modified, Tasks 1, 3, 4, 5)

src/platform/desktop/common/flash/
  flash_workflow.cpp                                   (modified, Tasks 1, 3, 4, 5)

src/platform/desktop/common/flash/legacy/ecu/
  flash_ecu_subaru_hitachi_m32r_can_operation.{h,cpp}  (deleted, Task 1)

src/platform/desktop/common/flash/legacy/tcu/
  flash_tcu_cvt_subaru_hitachi_m32r_can_operation.{h,cpp}    (deleted, Task 3)
  flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.{h,cpp}    (deleted, Task 4)
  flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.{h,cpp}    (deleted, Task 5)

scripts/check-legacy-flash-drain.py                    (modified, Tasks 1, 3, 4, 5 — REMAINING shrinks by one per task)
docs/flash-qualification-matrix.md                      (modified, Tasks 1, 3, 4, 5)
```

---

### Task 1: `FlashEcuSubaruHitachiM32rCan` — types, plan, executor

**Files:**
- Create: `src/backend/flash/ecu/subaru_hitachi_m32r_can_types.h`
- Create: `src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h`
- Create: `src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.cpp`
- Create: `src/backend/flash/ecu/subaru_hitachi_m32r_can_plan_test.cpp`
- Create: `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.h`
- Create: `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.cpp`
- Create: `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp`
- Modify: `scripts/check-legacy-flash-drain.py`
- Modify: `docs/flash-qualification-matrix.md`
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_m32r_can_operation.h`
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_m32r_can_operation.cpp`

**Interfaces:**
- Consumes: `uds::UdsClient::request(bytes::ByteView pdu, const uds::ExchangePolicy&, const ICancellationToken&) -> Result<bytes::Bytes>` (`src/backend/protocol/uds/uds_client.h`); `CanFlashUdsChannel(ICanFlashTransport&, std::uint32_t request_id, std::uint32_t response_id)` (`src/backend/flash/can_flash_uds_channel.h`); `SsmProtocol::calculateSeedKey(QByteArray/bytes seed, const uint16_t table[], const uint8_t transform[])` and `SsmProtocol::calculatePayload(...)` (`src/algorithms/protocol/ssm/ssm_protocol.h` — confirm the portable, non-Qt overload signature by reading that header before writing code; the legacy call sites above use the `QByteArray` `qt_compat` overload, but backend code must call the portable one).
- Produces: `fastecu::flash::SubaruHitachiM32rCanPlan` (types.h), `build_subaru_hitachi_m32r_can_plan(FlashOperation, std::string_view protocol_name, std::string_view mcu_type, std::optional<bytes::Bytes> image) -> Result<FlashPlan>`, `validate_subaru_hitachi_m32r_can_plan(const FlashPlan&) -> Status`, `class SubaruHitachiM32rCanExecutor final : public IFlashExecutor`.

- [ ] **Step 1: Read the portable SSM crypto signatures**

Run: open `src/algorithms/protocol/ssm/ssm_protocol.h` and confirm the exact portable (non-`QByteArray`) signatures for `calculateSeedKey` and `calculatePayload` — the wave-1 `subaru_hitachi_m32r_kline_executor.cpp` already calls them from portable code; copy its `#include` and call shape exactly.

- [ ] **Step 2: Write `subaru_hitachi_m32r_can_types.h`**

```cpp
#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_hitachi_m32r_can_operation.{h,cpp}. Single
// protocol variant, no vendor challenge, no capacity choice -- unlike
// MitsuColtM32rCanPlan this struct carries no operation-shaping fields, only
// the CAN identity every exchange needs.
struct SubaruHitachiM32rCanPlan
{
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false
};

} // namespace fastecu::flash
```

- [ ] **Step 3: Write the failing plan test**

```cpp
// subaru_hitachi_m32r_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::build_subaru_hitachi_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::SubaruHitachiM32rCanPlan;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "sub_ecu_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB_1block";

TEST(SubaruHitachiM32rCanPlan, RejectsUnknownProtocol)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Read,
                                                          "sub_ecu_hitachi_m32r_can_typo", kMcu,
                                                          std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruHitachiM32rCanPlan, RejectsMismatchedMcu)
{
    const auto plan =
        build_subaru_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, "MH8104", std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruHitachiM32rCanPlan, ReadPlanCoversTheFullRomFromZero)
{
    const auto plan =
        build_subaru_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0u);
    EXPECT_EQ(plan->transfer_region().length, 0x80000u);
    const auto& family = std::get<SubaruHitachiM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e0u);
    EXPECT_EQ(family.response_id, 0x7e8u);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_TRUE(plan->confirmations().empty());
    EXPECT_FALSE(plan->kernel().has_value());
}

TEST(SubaruHitachiM32rCanPlan, WritePlanCoversTheFullRomAndErasesItAll)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                          bytes::Bytes(0x80000, 0x00));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0u);
    EXPECT_EQ(plan->transfer_region().length, 0x80000u);
    ASSERT_EQ(plan->erase_regions().size(), 1u);
    EXPECT_EQ(plan->erase_regions()[0].start, 0u);
    EXPECT_EQ(plan->erase_regions()[0].length, 0x80000u);
}

TEST(SubaruHitachiM32rCanPlan, RejectsAWriteWhoseImageSizeIsWrong)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                          bytes::Bytes(0x60000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, HasSubstr("0x80000"));
}

TEST(SubaruHitachiM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::TestWrite, kProtocol,
                                                          kMcu, bytes::Bytes(0x80000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruHitachiM32rCanPlan, RejectsAWriteWithNoImage)
{
    const auto plan =
        build_subaru_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}
} // namespace
```

- [ ] **Step 4: Run the plan test to verify it fails to compile/link (targets do not exist yet)**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_m32r_can_plan_test`
Expected: FAIL (`no such target`).

- [ ] **Step 5: Write `subaru_hitachi_m32r_can_plan.h` and `.cpp`**

Model exactly on `src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.cpp` (single-variant family, `find_flash_device_index` + `flashdevices[index]` geometry cross-check). Header:

```cpp
#pragma once
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> build_subaru_hitachi_m32r_can_plan(FlashOperation operation,
                                                     std::string_view protocol_name,
                                                     std::string_view mcu_type,
                                                     std::optional<bytes::Bytes> image);
Status validate_subaru_hitachi_m32r_can_plan(const FlashPlan& plan);
} // namespace fastecu::flash
```

Source (`subaru_hitachi_m32r_can_plan.cpp`) — the geometry cross-check compares against the **Global Constraints** table above (`romsize=0x80000`, `numblocks=1`, `fblocks[0]={0, 0x80000}`):

```cpp
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"

#include <format>
#include <utility>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
using enum ErrorKind;

constexpr std::string_view kProtocol = "sub_ecu_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB_1block";
constexpr MemoryRegion kRom{0, 0x80000};

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol)
    {
        return fail(InvalidConfig,
                    std::format("Unsupported Subaru Hitachi M32R CAN protocol: {}", protocol));
    }
    const int index = find_flash_device_index(mcu);
    if (index < 0)
    {
        return fail(InvalidConfig, std::format("Unknown MCU type: {}", mcu));
    }
    if (mcu != kMcu)
    {
        return fail(InvalidConfig,
                    std::format("Protocol {} expects MCU {}; got {}", protocol, kMcu, mcu));
    }
    if (const flashdev_t& device = flashdevices[index];
        device.romsize != kRom.length || device.numblocks != 1 ||
        device.fblocks[0].start != kRom.start || device.fblocks[0].len != kRom.length)
    {
        return fail(InvalidConfig, "M32R one-block flash geometry is invalid");
    }
    return {};
}
} // namespace

Status validate_subaru_hitachi_m32r_can_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruHitachiM32rCan ||
        plan.transport() != TransportKind::CanIso15765)
    {
        return fail(InvalidConfig, "plan is not for Subaru Hitachi M32R CAN");
    }
    const auto *p = std::get_if<SubaruHitachiM32rCanPlan>(&plan.family_plan());
    if (p == nullptr || p->request_id != 0x7e0 || p->response_id != 0x7e8 ||
        p->bitrate != 500000 || p->extended_id)
    {
        return fail(InvalidConfig, "Hitachi M32R CAN wire parameters are invalid");
    }
    if (plan.transfer_region().start != kRom.start || plan.transfer_region().length != kRom.length)
    {
        return fail(InvalidConfig, "Hitachi M32R CAN transfer region is invalid");
    }
    if (plan.kernel())
    {
        return fail(InvalidConfig, "Hitachi M32R CAN plans are kernel-free");
    }
    if (plan.operation() == FlashOperation::TestWrite)
    {
        return fail(Unsupported, "test_write is not supported by this family");
    }
    if (plan.operation() == FlashOperation::Read && !plan.erase_regions().empty())
    {
        return fail(InvalidConfig, "read plans must not erase memory");
    }
    if (plan.operation() == FlashOperation::Write &&
        (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != 0 ||
         plan.erase_regions()[0].length != kRom.length))
    {
        return fail(InvalidConfig, "Hitachi M32R CAN erase region is invalid");
    }
    if (plan.operation() == FlashOperation::Write &&
        (!plan.image().has_value() || plan.image()->size() != kRom.length))
    {
        return fail(InvalidConfig, "ROM file must be exactly 0x80000 bytes");
    }
    return {};
}

Result<FlashPlan> build_subaru_hitachi_m32r_can_plan(FlashOperation operation,
                                                     std::string_view protocol_name,
                                                     std::string_view mcu_type,
                                                     std::optional<bytes::Bytes> image)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (operation == FlashOperation::TestWrite)
    {
        return fail(Unsupported, "test_write is not supported by this family");
    }
    if (operation == FlashOperation::Write && !image.has_value())
    {
        return fail(InvalidConfig, "Write plans must carry a ROM image");
    }
    if (operation == FlashOperation::Write && image->size() != kRom.length)
    {
        return fail(InvalidConfig,
                    std::format("ROM file must be exactly 0x80000 bytes; got 0x{:x} bytes",
                                image->size()));
    }
    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruHitachiM32rCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = kRom,
        .erase_regions = operation == FlashOperation::Write ? std::vector{kRom}
                                                             : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiM32rCanPlan{0x7e0, 0x7e8, 500000, false},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_hitachi_m32r_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
```

- [ ] **Step 6: Add `flash_types.h` wiring**

```cpp
// #include list gains:
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_types.h"

// FlashFamily enum gains, after SubaruDensoSh7055_02:
    // Step 5 tail, wave 3.
    SubaruHitachiM32rCan,

// FamilyPlan variant gains SubaruHitachiM32rCanPlan as an alternative.

// After the existing family_requires_kernel_v specializations:
template <>
inline constexpr bool family_requires_kernel_v<SubaruHitachiM32rCanPlan> = false;
```

- [ ] **Step 7: Add `subaru_hitachi_m32r_can_types` and `subaru_hitachi_m32r_can_plan`/`_plan_test` targets to `src/backend/flash/ecu/BUILD.bazel`**

Copy the `subaru_hitachi_m32r_kline_types`/`_plan`/`_plan_test` target shapes exactly (types has no deps beyond nothing; plan depends on `//src/backend/definitions:models`, `//src/backend/flash:flash_device_lookup`, `//src/backend/flash:flash_plan`, `//src/backend/flash:flash_validation`, `//src/backend/ports`).

- [ ] **Step 8: Run the plan test — verify it passes**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_m32r_can_plan_test`
Expected: PASS, all 6 cases.

- [ ] **Step 9: Commit**

```bash
git add src/backend/flash/ecu/subaru_hitachi_m32r_can_types.h \
        src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h \
        src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.cpp \
        src/backend/flash/ecu/subaru_hitachi_m32r_can_plan_test.cpp \
        src/backend/flash/ecu/BUILD.bazel \
        src/backend/flash/flash_types.h
git commit -m "feat: portable Subaru Hitachi M32R CAN plan (step 5 tail, wave 3)"
```

- [ ] **Step 10: Write the failing executor test — connect + read success**

```cpp
// subaru_hitachi_m32r_can_executor_test.cpp
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"
#include "src/backend/flash/flash_cancellation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;
using fastecu::flash::build_subaru_hitachi_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::ScriptedCanFlashTransport;
using fastecu::flash::SubaruHitachiM32rCanExecutor;

// Every request carries the 4-byte big-endian 0x7E0 envelope; every response
// the 0x7E8 reply id (legacy build sequence, connect_bootloader() lines
// 93-99 of flash_ecu_subaru_hitachi_m32r_can_operation.cpp).
bytes::Bytes request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e8);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

// Legacy connect_bootloader lines 91-260 (bench branch: received.at(5)==0xA0
// && received.at(6)==0x20 on the 0xA8 probe selects the "Bench Programming"
// arm at line 581). Scripts every exchange the bench arm performs: the
// already-running-OBK probe (miss), ECU ID / VIN / CAL ID / CVN diagnostic
// queries (all answered so their non-fatal log lines take the success path),
// the on-car probe (bench branch selected), session request, seed/key, and
// the alive check.
void scriptBenchConnect(ScriptedCanFlashTransport& transport)
{
    // "Checking if OBK is already running" -- 0xB7 probe, miss.
    transport.expectWrite(request({0xB7}));
    transport.queueRead(response({0x7F, 0xB7, 0x11}));

    // "Requesting ECU ID" -- 0xAA / 0xEA.
    transport.expectWrite(request({0xAA}));
    transport.queueRead(response(
        {0xEA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));

    // "Requesting VIN" -- 0x09 0x02 / 0x49 0x02.
    transport.expectWrite(request({0x09, 0x02}));
    transport.queueRead(response({0x49, 0x02, 'V', 'I', 'N'}));

    // "Requesting CAL ID" -- 0x09 0x04 / 0x49 0x04.
    transport.expectWrite(request({0x09, 0x04}));
    transport.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));

    // "Requesting CVN" -- 0x09 0x06 / 0x49 0x06.
    transport.expectWrite(request({0x09, 0x06}));
    transport.queueRead(response({0x49, 0x06, 0xAA, 0xBB}));

    // On-car session probe: 0xA8 0x00 0x00 0x00 0xD7. Response at.(5)==0xA0
    // and at.(6)==0x20 selects the bench branch.
    transport.expectWrite(request({0xA8, 0x00, 0x00, 0x00, 0xD7}));
    transport.queueRead(response({0x00, 0xA0, 0x20}));

    // Bench branch: session 0x10 0x43 / 0x50 0x43.
    transport.expectWrite(request({0x10, 0x43}));
    transport.queueRead(response({0x50, 0x43}));

    // Seed request: 0x27 0x01 / 0x67 0x01 <4-byte seed>.
    transport.expectWrite(request({0x27, 0x01}));
    transport.queueRead(response({0x67, 0x01, 0x11, 0x22, 0x33, 0x44}));

    // Seed key: 0x27 0x02 <4-byte key>. The test computes the expected key
    // itself, via the same SsmProtocol::calculateSeedKey call and the same
    // two tables the executor uses (kSeedKeyTable/kIndexTransformation,
    // transcribed from legacy generate_seed_key, lines 1352-1371) -- not a
    // recorded constant -- so a wrong table entry in the executor fails this
    // assertion instead of passing silently. This mirrors
    // mitsu_colt_m32r_can_executor_test.cpp's own
    // `MitsuColtCan::seedKey(kSeed)` call.
    constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
        0x90A1, 0x2F92, 0xDE3C, 0xCDC0, 0x1A99, 0x437C, 0xF91B, 0xDB57,
        0x96BA, 0xDE10, 0xFCAF, 0x3F31, 0xF47F, 0x0BB6, 0x16E9, 0x4645};
    constexpr std::array<std::uint8_t, 32> kIndexTransformation{
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    const bytes::Bytes seed{0x11, 0x22, 0x33, 0x44};
    const bytes::Bytes key = SsmProtocol::calculateSeedKey(seed, kSeedKeyTable.data(),
                                                            kIndexTransformation.data());
    bytes::Bytes keyRequest{0x27, 0x02};
    keyRequest.insert(keyRequest.end(), key.begin(), key.end());
    transport.expectWrite(request(keyRequest));
    transport.queueRead(response({0x67, 0x02}));

    // Jump to kernel: 0x10 0x42 / 0x50 0x42.
    transport.expectWrite(request({0x10, 0x42}));
    transport.queueRead(response({0x50, 0x42}));

    // Kernel-alive check: 0x34 0x04 0x33 0x00000008 0x0000 / 0x74 0x20 0x01 0x04.
    transport.expectWrite(request({0x34, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}));
    transport.queueRead(response({0x74, 0x20, 0x01, 0x04}));
}

TEST(SubaruHitachiM32rCanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    SubaruHitachiM32rCanExecutor executor;
    auto foreign = build_subaru_hitachi_m32r_can_plan(FlashOperation::Read, "wrong", "wrong",
                                                       std::nullopt);
    // Constructing a genuinely foreign plan follows the RejectsAPlanFromAnotherFamily
    // pattern in mitsu_colt_m32r_can_executor_test.cpp -- hand-build a
    // DensoSh705xEepromCan plan via validate_and_build directly here.
}
// ... (remaining structure below)
} // namespace
```

Because this file's full test suite is long, do not hand-write every case inline in this plan — instead **write the tests in this order**, using `mitsu_colt_m32r_can_executor_test.cpp` and `subaru_hitachi_m32r_kline_executor_test.cpp` as the two structural templates (CAN scripted transport + K-Line-style single-variant plan, respectively):

1. `RejectsAPlanFromAnotherFamilyBeforeAnyIo` (copy the Colt test's shape).
2. `ConnectAndReadReturnsTheFullRomFromAddressZero` — `scriptBenchConnect` above, then script the full `0x80000`-byte, 256-byte-chunk `0xB7` dump sweep (helper `scriptFlashDump(transport, start=0, length=0x80000, pagesize=0x100, fill)`, modeled on Colt's `scriptFlashRead`), then the stop command `0x37`/`0x77`.
3. `ReadReportsAnEmptyReplyAsTimeout` — script only the OBK-probe miss and the first ECU-ID query timing out (`queue_no_frame()`), assert `ErrorKind::Timeout`. (The legacy ID-query failures are logged and non-fatal, but a genuinely empty/missing reply from the transport at the OBK-probe step itself is what a real timeout looks like — confirm which specific request's absent reply should be scripted by re-reading `connect_bootloader` lines 91-113 before writing this test, since the probe read uses `serial_read_short_timeout` (200 ms) with no error handling on empty digitally at all — only failures that DO error-return matter for this test.)
4. `ReadPropagatesADisconnectedTransport` — `queue_error(ErrorKind::Disconnected, ...)` at the seed-key exchange.
5. `ReadStopsWhenCancelledBeforeAnyExchange` — trip cancellation before `execute()`, assert zero writes.
6. `ReadStopsAtTheNextChunkWhenCancelledMidRead` — mirror Colt's `CancelAfterFirstChunkSink` pattern, tripping cancellation once the first dump chunk's progress is reported.
7. `WriteErasesAndWritesTheFullRomInOneReflashBlock` — `scriptBenchConnect`, then the `0x34 0x04 0x33 ...` "set flash start & length" exchange (positive response `0x74`), then 2048 `0xB6` write-chunk exchanges (helper `scriptReflashBlock(transport, start=0, data, chunkSize=256)`, positive response `0xF6` each), then the close-block retry loop (script the FIRST `0x37`/`0x77` exchange to succeed immediately — the retry-tolerant loop is exercised separately in test 9), then the checksum verify (`0x31 0x01 0x02 0x02 0x01`, positive intermediate `0x7F 0x31 0x78`, then delayed final `0x71 0x01 0x02`).
8. `WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo` — hand-built plan with a mismatched image, per Colt's `handBuiltWritePlan` pattern.
9. `WriteToleratesUpToFiveFailedCloseAttemptsBeforeSucceeding` — legacy `reflash_block`'s close-block loop retries up to 6 times and proceeds to checksum verification even if every attempt reports something other than `0x77` (the loop's `connected` flag is read nowhere after the loop — legacy lines 1188-1210). Script 5 non-`0x77` responses followed by a 6th `0x77`, assert success, this pins the retry-tolerant quirk explicitly.
10. `WriteStopsWhenTheEraseIsRejected` — negative response on the first `0xB6` chunk (`0x7F 0xB6 <nrc>`), assert `ErrorKind::BadResponse` and zero further chunk writes.
11. `RefusesATestWritePlanRatherThanWritingForReal` — `writePlanGranting`-style hand-built `TestWrite` plan; assert `ErrorKind::Unsupported`, zero writes beyond the connect handshake.

Write test 1-2 first (Steps 10-13 below cover the connect+read pair); the remaining nine follow the same write/run/pass cycle one at a time as part of Steps 14+ before Step 15's final full-suite run.

- [ ] **Step 11: Run the executor test — verify it fails to compile (target/executor do not exist yet)**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_m32r_can_executor_test`
Expected: FAIL (no such target).

- [ ] **Step 12: Write `subaru_hitachi_m32r_can_executor.h`**

```cpp
#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruHitachiM32rCanExecutor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport,
                                         IClock& clock, const ICancellationToken& cancellation,
                                         IEventSink& events) override;
};
} // namespace fastecu::flash
```

- [ ] **Step 13: Write `subaru_hitachi_m32r_can_executor.cpp`**

Structure it exactly like `mitsu_colt_m32r_can_executor.cpp`: an anonymous namespace with `Ctx`, `info`/`error` helpers, `report_exchange_failure`, then free functions `connect_bootloader`, `dump_flash_range`, `unlock_and_reflash_block`, `write_mem`, each citing `flash_ecu_subaru_hitachi_m32r_can_operation.cpp:<lines>`. Crypto tables (legacy `generate_seed_key`/`encrypt_payload`/`decrypt_payload`, lines 1352-1422):

```cpp
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0x90A1, 0x2F92, 0xDE3C, 0xCDC0, 0x1A99, 0x437C, 0xF91B, 0xDB57,
    0x96BA, 0xDE10, 0xFCAF, 0x3F31, 0xF47F, 0x0BB6, 0x16E9, 0x4645};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x14CA, 0x77F4, 0x973C, 0xF50E};
constexpr std::array<std::uint16_t, 4> kDecryptTable{0xF50E, 0x973C, 0x77F4, 0x14CA};
// kIndexTransformation: the shared 32-byte table from Global Constraints,
// identical to every other family in this wave.
```

Key differences from Colt to encode faithfully:

- **`connect_bootloader`** first probes `0xB7` (not a diagnostic-session SID) for "OBK already running" (lines 93-112); on miss, issues the four non-fatal ID/VIN/CAL/CVN queries (lines 115-257, each independently `info`-logged on success and `error`-logged — never returning early — on failure or empty reply, exactly mirroring legacy's non-fatal handling); then the on-car probe (`0xA8 00 00 00 D7`, lines 262-279) whose response selects between the on-car arm (lines 281-579, not ported — see the design's on-car `Unsupported` scope decision, enforced in the plan builder, not here) and the bench arm (lines 581-753, the only arm this executor drives). The bench arm is: session `0x10 0x43` (non-fatal on mismatch, lines 591-612 — **note the asymmetry with the vendor-key check below: this one does early-return on both the empty-reply and mismatch branches**, transcribe exactly), seed `0x27 0x01` (fatal on mismatch/empty, lines 616-642), seed key `0x27 0x02` (fatal, lines 660-687), jump `0x10 0x42` (fatal, lines 690-717), alive check `0x34 0x04 0x33 00 00 00 08 00 00` expecting `0x74 0x20 0x01 0x04` (fatal, lines 720-752).
- **`dump_flash_range`** (legacy `read_mem`, lines 767-963): send `0x35 0x04 0x33 00 00 00 08 00 00` once (non-fatal on mismatch — legacy only logs, lines 809-819, never returns early), then loop `0xB7 00 <addr3>` at 256-byte pages until the full `0x80000` region is covered, decrypting each `0xF7`-prefixed payload via `SsmProtocol::calculatePayload` with the **decrypt** table (`keytogenerateindex = {0xF50E, 0x973C, 0x77F4, 0x14CA}`, note this is the reverse order of the encrypt table below), checked and cancellation-polled once per chunk (top of loop, mirroring legacy's `stopRequested()` at line 840). Stop with `0x37`/`0x77` (lines 928-955, fatal on mismatch here, unlike the tolerant `reflash_block` close loop below).
- **`unlock_and_reflash_block`** (legacy `reflash_block`, lines 1056-1272): one call for the whole ROM (`numblocks=1`). `0x34 0x04 0x33 00 00 00 08 00 00` / `0x74` (fatal, lines 1094-1127), then 2048 `0xB6 <addr3> <256 encrypted bytes>` / `0xF6` chunk exchanges (fatal per chunk, lines 1130-1176) where the 256-byte payload is `SsmProtocol::calculatePayload(fullRom, len, encryptTable, indextransformation)` sliced at the chunk's address (**encrypt** table `{0x14CA, 0x77F4, 0x973C, 0xF50E}` — the whole `0x80000`-byte ROM is encrypted once up front, exactly as legacy's `write_mem` does at line 986, not per-chunk), then the close-block loop: up to 6 attempts of `0x37`, tolerant of any non-`0x77` response (lines 1180-1210, **the loop's `connected` flag being false after 6 attempts is never checked — do not add a check the legacy code doesn't have**), then checksum verify `0x31 0x01 0x02 0x02 0x01` expecting first `0x7F 0x31 0x78` (fatal on mismatch, lines 1230-1245) then, after a further read, `0x71 0x01 0x02` (fatal on mismatch, lines 1252-1266).
- **`write_mem`**: `numblocks=1` so this is exactly one `unlock_and_reflash_block` call over the full `{0, 0x80000}` region — no erase-then-loop split like Colt (there is no separate `erase_memory()` call in the ECU Hitachi CAN class; `RoutineControl` erase happens implicitly inside `reflash_block`'s checksum-verify step, unlike the TCU families below which call an explicit `erase_mem()` first). **Confirm this by re-reading legacy `write_mem` (lines 971-1049) before implementing** — it calls `reflash_block` directly per modified block with no preceding erase call, unlike every TCU family in this wave.

`execute()` follows Colt's shape: `check_family_transport_match(plan, FlashFamily::SubaruHitachiM32rCan, TransportKind::CanIso15765)`, `validate_subaru_hitachi_m32r_can_plan`, cancellation check, downcast to `ICanFlashTransport`, configure/open, construct `CanFlashUdsChannel`/`UdsClient`, `connect_bootloader`, then read-or-write per `plan.operation()`.

- [ ] **Step 14: Add the remaining executor test cases from Step 10's list, one at a time, running the suite after each**

Run after each: `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_m32r_can_executor_test`

- [ ] **Step 15: Add `subaru_hitachi_m32r_can_executor`/`_executor_test` targets to `BUILD.bazel`**

Copy `mitsu_colt_m32r_can_executor`'s dep shape (`//src/algorithms/protocol`, `//src/algorithms/protocol/ssm`, `//src/algorithms/protocol/uds`, `//src/backend/flash:can_flash_uds_channel`, `//src/backend/flash:flash_executor`, `//src/backend/flash:flash_plan`, `//src/backend/flash:flash_types`, `//src/backend/ports`, `//src/backend/protocol/uds:uds_client`).

- [ ] **Step 16: Run the full executor test suite — verify all pass**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_m32r_can_executor_test`
Expected: PASS, all 11 cases.

- [ ] **Step 17: Wire the desktop workflow**

In `src/platform/desktop/common/flash/flash_workflow.cpp`, add a class following `ColtWorkflow`'s shape exactly but with `plan_->confirmations()` naturally empty (no `stage_` branching needed beyond the single `Begin` prompt):

```cpp
class SubaruHitachiM32rCanWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruHitachiM32rCanWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)),
          plan_(build_subaru_hitachi_m32r_can_plan(request_.operation, request_.protocol,
                                                    request_.mcu, std::move(request_.image)))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!plan_)
        {
            return FlashFailureStep{plan_.error()};
        }
        if (failure_)
        {
            return FlashFailureStep{std::move(*failure_)};
        }
        if (terminal_)
        {
            return completed(outcome_, std::move(accepted_));
        }
        if (!began_)
        {
            began_ = true;
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (!attempted_)
        {
            attempted_ = true;
            FlashPlan plan = std::move(*plan_);
            return FlashAttempt{std::move(plan), std::make_unique<SubaruHitachiM32rCanExecutor>(),
                                std::make_unique<DesktopCanFlashTransport>(request_.serial),
                                std::make_unique<QtClock>()};
        }
        return completed(outcome_, std::move(accepted_));
    }

    void submit(FlashPromptResponse response) override
    {
        if (response != FlashPromptResponse::Accept)
        {
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        terminal_ = true;
        if (result.success)
        {
            outcome_ = FlashWorkflowOutcome::Succeeded;
            accepted_ = std::move(result.read_bytes);
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
        else
        {
            outcome_ = FlashWorkflowOutcome::Failed;
            failure_ = Error{result.error_kind, std::move(result.error_detail)};
        }
    }

  private:
    FlashWorkflowRequest request_;
    Result<FlashPlan> plan_;
    bool began_ = false;
    bool attempted_ = false;
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> accepted_;
    std::optional<Error> failure_;
};
```

Add `SubaruHitachiM32rCan` to `Route::Kind`, add `{"sub_ecu_hitachi_m32r_can", SubaruHitachiM32rCan}` to `kRoutes` **before** the `on-car`/generic routes are ever added in a later wave (none exist yet — this is a plain new row), and add the `case` in `tryCreate`.

- [ ] **Step 18: Shrink the drain ratchet**

In `scripts/check-legacy-flash-drain.py`, remove `"ecu/flash_ecu_subaru_hitachi_m32r_can_operation.cpp"` from `REMAINING`.

- [ ] **Step 19: Delete the legacy family**

```bash
git rm src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_m32r_can_operation.h \
       src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_m32r_can_operation.cpp
```

Remove any now-dead include/reference in `src/platform/desktop/common/flash/legacy/ecu/BUILD.bazel` if the glob does not already exclude the deleted files automatically (it does, per the tail design's "legacy `BUILD.bazel` needs no edit per family — its `srcs`/`MOC_HDRS` are globs" — verify this by building afterward rather than editing preemptively).

- [ ] **Step 20: Flip the matrix row**

In `docs/flash-qualification-matrix.md`, update the `FlashEcuSubaruHitachiM32rCan` row: `portable` → `yes`; `automated_evidence` → `subaru_hitachi_m32r_can_plan_test, subaru_hitachi_m32r_can_executor_test @ Wave 3`; `hardware_status` stays `unqualified` → `experimental`; `notes` gains: "On-car programming branch (CAN ids beyond 0x7E0/0x7E8) is out of scope for this port — `build_subaru_hitachi_m32r_can_plan` targets the bench path only; on-car mode is not exposed by any current `protocols.cfg` entry, so this is a scope boundary, not a behavior regression."

- [ ] **Step 21: Run the full gate**

Run:
```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
```
Expected: all green; `//:legacy_flash_drain`'s `REMAINING` count is now 21.

- [ ] **Step 22: Commit**

```bash
git add -A
git commit -m "feat: portable Subaru Hitachi M32R CAN executor + workflow (step 5 tail, wave 3)"
```

---

### Task 2: Request code review before continuing the wave

- [ ] **Step 1: Invoke `superpowers:requesting-code-review` on Task 1's diff before starting Task 3.**

Wave 1 and 2 each landed as separate, individually-reviewed PRs; this plan produces the same shape as one continuous session, so pause here for review rather than batching all four families into one uncriticized diff. Apply any resulting fixes before proceeding.

---

### Task 3: `FlashTcuCvtSubaruHitachiM32rCan` — types, plan, executor (dead-code replacement + address-bug divergence)

**Files:**
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_types.h`
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h`
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.cpp`
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan_test.cpp`
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.h`
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp`
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/flash_types.h`, `src/backend/flash/ecu/BUILD.bazel`, `src/platform/desktop/common/flash/flash_workflow.cpp`, `scripts/check-legacy-flash-drain.py`, `docs/flash-qualification-matrix.md`
- Delete: `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.{h,cpp}`

**Interfaces:**
- Consumes: same `UdsClient`/`CanFlashUdsChannel`/`SsmProtocol` surface as Task 1.
- Produces: `SubaruTcuCvtHitachiM32rCanPlan`, `build_subaru_tcu_cvt_hitachi_m32r_can_plan(...)`, `validate_subaru_tcu_cvt_hitachi_m32r_can_plan(...)`, `class SubaruTcuCvtHitachiM32rCanExecutor final : public IFlashExecutor`.

- [ ] **Step 1: Re-read `flash_tcu_cvt_subaru_hitachi_m32r_can_operation.cpp`'s `connect_bootloader` (lines 87-408), `read_mem` (lines 499-719), `write_mem`/`reflash_block`/`erase_mem` (lines 727-1039), and `generate_seed_key`/`encrypt_payload`/`decrypt_payload` (lines 1051-1122) in full before writing any code — `hack_words()` (lines 415-492) is explicitly NOT ported (see the design's dead-code decision); every other private method IS.**

This confirms the exact exchange order and byte layout this task transcribes. Do not rely solely on this plan document's summary below for byte values — re-derive them from the cited lines, per the umbrella's fidelity discipline.

- [ ] **Step 2: Write `subaru_tcu_cvt_hitachi_m32r_can_types.h`**

```cpp
#pragma once
#include <cstdint>

namespace fastecu::flash
{
// Legacy: flash_tcu_cvt_subaru_hitachi_m32r_can_operation.{h,cpp}. execute()
// calls the dead hack_words() (always STATUS_ERROR); this plan/executor pair
// ports the real, previously-unreachable connect_bootloader/read_mem/
// write_mem logic instead -- see the wave-3 design's "Deliberate divergence"
// section.
struct SubaruTcuCvtHitachiM32rCanPlan
{
    std::uint32_t request_id;  // 0x7e1
    std::uint32_t response_id; // 0x7e9
    int bitrate;               // 500000
    bool extended_id;          // false
};
} // namespace fastecu::flash
```

- [ ] **Step 3: Write the failing plan test**

Follow Task 1 Step 3's shape exactly, substituting: protocol `sub_tcu_cvt_hitachi_m32r_can`, mcu `M32R_512KB`, read region `{0x8000, 0x78000}` (the resolved-clamp target, not the literal underflowed legacy arithmetic — see Global Constraints), write region `{0x8000, 0x78000}`, image size `0x80000`. Add one extra case:

```cpp
TEST(SubaruTcuCvtHitachiM32rCanPlan, ReadRegionIsTheFloorClampedWindowNotTheLiteralUnderflow)
{
    // Legacy read_mem computes start_addr - 0x00100000 with start_addr == 0,
    // which underflows uint32_t to 0xFFF00000 and bypasses the
    // "< 0x8000" floor clamp entirely -- this path never executed in
    // production (execute() called hack_words(), never read_mem()). This
    // plan targets the clamp's evident intent (0x8000) rather than
    // reproducing an address computation nothing ever observed on the wire.
    const auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Read,
                                                                  "sub_tcu_cvt_hitachi_m32r_can",
                                                                  "M32R_512KB", std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0x8000u);
    EXPECT_EQ(plan->transfer_region().length, 0x78000u);
}
```

- [ ] **Step 4: Run the plan test — verify it fails to compile**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_plan_test`
Expected: FAIL (no such target).

- [ ] **Step 5: Write `subaru_tcu_cvt_hitachi_m32r_can_plan.h`/`.cpp`**

Same shape as Task 1 Step 5, with: `kProtocol = "sub_tcu_cvt_hitachi_m32r_can"`, `kMcu = "M32R_512KB"`, geometry check against `romsize=0x80000, numblocks=11, fblocks[0]={0,0x4000}` (spot-check block 0 only, matching `subaru_hitachi_m32r_kline_plan.cpp`'s precedent of checking `fblocks[0]`), `kReadRegion = MemoryRegion{0x8000, 0x78000}`, `kWriteRegion = MemoryRegion{0x8000, 0x78000}` (identical to the read region for this family — unlike MH8111 below), `family_plan = SubaruTcuCvtHitachiM32rCanPlan{0x7e1, 0x7e9, 500000, false}`.

- [ ] **Step 6: Wire `flash_types.h`** — add `SubaruTcuCvtHitachiM32rCan` enum value, `SubaruTcuCvtHitachiM32rCanPlan` variant alternative, `family_requires_kernel_v<SubaruTcuCvtHitachiM32rCanPlan> = false`.

- [ ] **Step 7: Add BUILD.bazel targets, run the plan test, verify PASS, commit.**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_plan_test`

```bash
git add src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_types.h \
        src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h \
        src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.cpp \
        src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan_test.cpp \
        src/backend/flash/ecu/BUILD.bazel src/backend/flash/flash_types.h
git commit -m "feat: portable Subaru TCU CVT Hitachi M32R CAN plan (step 5 tail, wave 3)"
```

- [ ] **Step 8: Write the failing executor test — connect + read success**

Follow Task 1 Step 10's structural pattern. Key differences to encode:

```cpp
// TCU exchanges use the 0x7e1/0x7e9 pair.
bytes::Bytes request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e1);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e9);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

// Legacy connect_bootloader lines 100-408: kernel-alive probe (0x31 0x02
// 0x02 0x01 / 0x71 0x02 0x02 0x03), then (on miss) the diagnostic queries
// (0xAA and 0x09 0x04 -- both sent, per legacy, on the ECU's *primary*
// arb-id envelope: transcribe this exactly, it is 0x7E0 requests inside a
// connection configured for 0x7E1/0x7E9, a legacy quirk worth its own
// comment citing lines 138-211), session 0x10 0x03 (fatal on mismatch,
// lines 216-242), session 0x10 0x43 (non-fatal, lines 244-265), seed 0x27
// 0x01 (fatal, lines 267-293), seed key 0x27 0x02 (fatal, lines 305-333),
// jump 0x10 0x02 sent on 0x7e1 (fatal, lines 339-365), alive re-check 0x31
// 0x02 0x02 0x01 expecting 0x71 0x02 0x02 0x03 sent on 0x7e1 (fatal, lines
// 373-401).
```

Write these tests, one at a time, checking the full suite after each:

1. `RejectsAPlanFromAnotherFamilyBeforeAnyIo`
2. `ConnectSkipsTheRestWhenKernelAlreadyRunning` — script only the alive probe returning `0x71 0x02 0x02 0x03`; assert success with zero further writes (legacy lines 117-129, `return STATUS_SUCCESS` immediately).
3. `ConnectFullSequenceWhenKernelNotRunning` — the full sequence above.
4. `ReadReturnsTheFloorClampedWindowPaddedWithZero` — script the full `dump_flash_range` sweep over `{0x8000, 0x78000}` and assert the returned bytes are `0x8000` zero bytes followed by the dumped payload (legacy lines 705-712's `0x00` padding, transcribed exactly — **not** `0xFF`, unlike the two Mitsu families in Tasks 4-5).
5. `ReadReportsAnEmptyReplyAsTimeout`
6. `ReadStopsWhenCancelled`
7. `WriteErasesThenFlashesEightBlocksOfSixtyFourKib` — `erase_mem()` is a **separate, explicit call** in this family (legacy lines 991-1039: `0x31 0x02 0x01 <0xff 0xff 0xff 0xff>` / fatal-checked `0x31 0x02 0x01`), unlike Task 1's ECU family which has no standalone erase. Script the erase exchange, then eight `reflash_block` calls (one per `0x10000`-byte block from `0x8000` to `0x80000`), each: `0x34 0x04 0x33 <addr3> <len3>` / `0x74` (fatal, lines 862-880), `maxblocks=512` chunks of `0xB6 <addr3> <128 bytes>` at 128-byte granularity **with no response check inside the loop at all** (legacy lines 907-908 reads but never inspects the reply — transcribe as: read the reply, discard it, do not fail on any content, but DO propagate a transport-level `Result` failure/timeout from the read itself), close `0x37` (fatal, lines 926-941), checksum `0x31 0x02 0x02 0x01` expecting `0x71 0x02 0x02` prefix (fatal, lines 953-976).
8. `WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo`
9. `RefusesATestWritePlanRatherThanWritingForReal`

- [ ] **Step 9: Run the executor test — verify it fails to compile**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_executor_test`

- [ ] **Step 10: Write `subaru_tcu_cvt_hitachi_m32r_can_executor.h`/`.cpp`**

Same structural shape as Task 1 Step 12-13. Crypto tables (both from legacy `generate_seed_key`/`encrypt_payload`/`decrypt_payload`, lines 1051-1122):

```cpp
// Seed key.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
    0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
// Encrypt (write payload).
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x3B61, 0x8BEF, 0x9E51, 0x1075};
// Decrypt (read payload) -- reverse order of kEncryptTable, same values.
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x1075, 0x9E51, 0x8BEF, 0x3B61};
// Shared by all four wave-3 families and wave-1 Hitachi K-Line.
constexpr std::array<std::uint8_t, 32> kIndexTransformation{
    0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
```

`dump_flash_range` targets `{0x8000, 0x78000}` unconditionally (the resolved clamp, per the plan). `unlock_and_reflash_block` takes 128-byte chunks and, per legacy lines 907-908, does **not** inspect the per-chunk response content at all — only a transport-level error (timeout/disconnect/cancellation) from the read itself should propagate; a well-formed-but-wrong-content reply must NOT fail this exchange (this is the family's own, distinct tolerance quirk, separate from the close-loop tolerance shared with Task 1).

- [ ] **Step 11: Add remaining executor tests one at a time from Step 8's list; run the suite after each.**

- [ ] **Step 12: Add BUILD.bazel targets; run full executor test suite; verify PASS.**

- [ ] **Step 13: Wire the desktop workflow** — extend `Route::Kind`/`kRoutes`/`tryCreate` with `SubaruTcuCvtHitachiM32rCan` → `{"sub_tcu_cvt_hitachi_m32r_can", ...}`, reusing the Task 1 Step 17 workflow class shape verbatim (parametrize the shared shape by plan-builder + executor type if convenient, or duplicate the ~50-line class — either is acceptable; do not introduce a runtime branch inside one class for what are compile-time-distinct executor types).

- [ ] **Step 14: Shrink the drain ratchet** — remove `"tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.cpp"` from `REMAINING`.

- [ ] **Step 15: Delete the legacy family.**

```bash
git rm src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.h \
       src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.cpp
```

- [ ] **Step 16: Flip the matrix row**

`portable` → `yes`; `automated_evidence` → `subaru_tcu_cvt_hitachi_m32r_can_plan_test, subaru_tcu_cvt_hitachi_m32r_can_executor_test @ Wave 3`; `hardware_status` → `experimental`; `notes` gains: "Legacy `execute()` called the always-failing `hack_words()`, not `connect_bootloader()` — this family has never run in production. This port uses the real, previously-unreachable `connect_bootloader`/`read_mem`/`write_mem` logic instead, giving the family a working portable path for the first time; this is the wave's deliberate divergence. A second, related divergence: legacy `read_mem`'s `start_addr - 0x00100000` bias underflows to `0xFFF00000` for the only `start_addr` ever passed (0), bypassing its own `< 0x8000` floor-clamp — this port targets the clamp's evident intent (`{0x8000, 0x78000}`) rather than the never-observed underflowed address. No hardware qualification is claimed."

- [ ] **Step 17: Run the full gate; commit.**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
git add -A
git commit -m "feat: portable Subaru TCU CVT Hitachi M32R CAN executor + workflow (step 5 tail, wave 3)"
```

Expected: `//:legacy_flash_drain`'s `REMAINING` count is now 20.

---

### Task 4: `FlashTcuCvtSubaruMitsuMH8111Can` — types, plan, executor (read/write region asymmetry)

**Files:** same five-file-plus-wiring shape as Tasks 1 and 3, named `subaru_tcu_cvt_mitsu_mh8111_can_*`.

**Interfaces:**
- Produces: `SubaruTcuCvtMitsuMh8111CanPlan`, `build_subaru_tcu_cvt_mitsu_mh8111_can_plan(...)`, `validate_subaru_tcu_cvt_mitsu_mh8111_can_plan(...)`, `class SubaruTcuCvtMitsuMh8111CanExecutor final : public IFlashExecutor`.

- [ ] **Step 1: Re-read `flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.cpp` in full (all 984 lines) before writing code, in particular `reflash_block` lines 636-826 and its `maxblocks`/`end_addr` computation at lines 661-663.**

- [ ] **Step 2: Write `subaru_tcu_cvt_mitsu_mh8111_can_types.h`**

```cpp
#pragma once
#include <cstdint>

namespace fastecu::flash
{
struct SubaruTcuCvtMitsuMh8111CanPlan
{
    std::uint32_t request_id;  // 0x7e1
    std::uint32_t response_id; // 0x7e9
    int bitrate;               // 500000
    bool extended_id;          // false
};
} // namespace fastecu::flash
```

- [ ] **Step 3: Write the failing plan test**, asserting the read/write region asymmetry explicitly:

```cpp
TEST(SubaruTcuCvtMitsuMh8111CanPlan, ReadCoversTheLowerWindowWriteCoversTheUpperBlock)
{
    // MH8111's flash geometry is {0,0x40000},{0x40000,0x20000},{0x60000,0x20000},
    // {0x80000,0x100000} (kernelmemorymodels.h fblocks_MH8111); block_modified
    // skips blocks 0-2, so write_mem's only reflash_block call targets block 3:
    // {0x80000, 0x100000}. read_mem hardcodes {0x8000, 0x78000} regardless.
    // These two regions do NOT overlap -- a genuine legacy asymmetry, not a
    // copy/paste error, preserved exactly rather than "fixed" into symmetry.
    const auto readPlan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(
        FlashOperation::Read, "sub_tcu_cvt_mitsu_mh8111_can", "MH8111", std::nullopt);
    ASSERT_TRUE(readPlan.has_value()) << readPlan.error().detail;
    EXPECT_EQ(readPlan->transfer_region().start, 0x8000u);
    EXPECT_EQ(readPlan->transfer_region().length, 0x78000u);

    const auto writePlan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(
        FlashOperation::Write, "sub_tcu_cvt_mitsu_mh8111_can", "MH8111",
        bytes::Bytes(0x180000, 0x00));
    ASSERT_TRUE(writePlan.has_value()) << writePlan.error().detail;
    EXPECT_EQ(writePlan->transfer_region().start, 0x80000u);
    EXPECT_EQ(writePlan->transfer_region().length, 0x100000u);
    ASSERT_TRUE(writePlan->image().has_value());
    EXPECT_EQ(writePlan->image()->size(), 0x180000u);
}
```

Plus the standard rejection cases (unknown protocol, mismatched mcu, wrong image size — `0x180000` exactly for write, per the family's true declared capacity `3*512*1024`, not the read window size), mirroring Task 1 Steps 3.

- [ ] **Step 4: Run — verify fails to compile.**

- [ ] **Step 5: Write `subaru_tcu_cvt_mitsu_mh8111_can_plan.h`/`.cpp`**

Same shape as before, with `kMcu = "MH8111"`, geometry check `romsize=0x180000, numblocks=4, fblocks[3]={0x80000,0x100000}` (check block 3 specifically here, since it — not block 0 — is the block this family actually writes; also check `fblocks[0]={0,0x40000}` for full-table sanity), `kReadRegion = {0x8000, 0x78000}`, `kWriteRegion = {0x80000, 0x100000}`, write image size requirement `0x180000` (the full declared capacity, since `reflash_block` indexes the encrypted buffer at absolute offsets up to `0x180000`), `family_plan = SubaruTcuCvtMitsuMh8111CanPlan{0x7e1, 0x7e9, 500000, false}`.

- [ ] **Step 6: Wire `flash_types.h`**, add BUILD.bazel targets, run plan test, verify PASS, commit.

```bash
git commit -m "feat: portable Subaru TCU CVT Mitsu MH8111 CAN plan (step 5 tail, wave 3)"
```

- [ ] **Step 7: Write the failing executor test — connect + read + write success**

Structural pattern from Task 1/3, with the `0x7e1`/`0x7e9` envelope (shared with Task 3). Connect sequence (legacy `connect_bootloader`, lines 81-337): **no kernel-alive pre-check** in this family (unlike Task 3) — it always runs the full sequence: ECU ID `0xAA` (non-fatal, lines 94-133), CAL ID `0x09 0x04` (non-fatal, lines 135-168), session `0x10 0x43` (non-fatal, lines 173-197), seed `0x27 0x01` (fatal, lines 199-225), seed key `0x27 0x02` (fatal, lines 236-263), jump `0x10 0x42` (fatal, lines 267-295), alive check `0x31 0x02 0x02 0x01` expecting `0x71 0x02 0x02 0x03` (fatal, lines 298-331).

Write these tests, one at a time:

1. `RejectsAPlanFromAnotherFamilyBeforeAnyIo`
2. `ConnectFullSequenceEveryTime` — proves there is no alive-skip shortcut (unlike Task 3), asserting every one of the 7 exchanges above is sent.
3. `ReadReturnsTheLowerWindowPaddedWithFF` — dump sweep over `{0x8000, 0x78000}`, decrypt with `kDecryptTable = {0x6587, 0x4492, 0xa8b4, 0x7bf2}`, assert result is `0x8000` bytes of `0xFF` (not `0x00` — this family pads differently from Task 3) followed by the dump.
4. `ReadReportsAnEmptyReplyAsTimeout`
5. `ReadStopsWhenCancelled`
6. `WriteErasesThenFlashesTheUpperBlockAtAddressAbove0x80000` — erase `0x31 0x01 0x02 0x01 0x0f 0xff 0xff 0xff` retried up to 20 times until `0x71 0x01 0x02` (legacy `erase_mem` lines 833-892 — **fatal after 20 failed attempts**, unlike Task 3's single-shot erase), then `reflash_block` for the single block at `0x80000`, length `0x100000`: `0x34 0x04 0x33 00 00 00 <len3>` retried up to 6 times until any non-empty reply (legacy lines 686-705 — **content of the reply is never checked**, only that it arrived; retranscribe exactly, do not add a `0x74` check the legacy code lacks after the retry loop other than the `try_count==6` bailout), then `maxblocks = 0x100000/256 = 4096` chunks of `0xB6 <addr3> <256 bytes>` with **no response check inside the loop** (legacy lines 734-736: read but discard), close `0x37` retried up to 20 times until `0x77` (fatal after 20, lines 752-782), checksum `0x31 0x01 0x02 0x02 0x01` retried up to 20 times until `0x71 0x01 0x02` (fatal after 20, lines 798-825).
7. `WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo`
8. `RefusesATestWritePlanRatherThanWritingForReal`

- [ ] **Step 8: Write `subaru_tcu_cvt_mitsu_mh8111_can_executor.h`/`.cpp`**

Crypto tables (legacy lines 904-977):

```cpp
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
    0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x7bf2, 0xa8b4, 0x4492, 0x6587};
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x6587, 0x4492, 0xa8b4, 0x7bf2};
// kIndexTransformation: identical to every other family in this wave.
```

Transcribe the retry-until-N-attempts pattern for `erase_mem` (20 attempts, fatal), the `reflash_block` setup exchange (6 attempts, content-blind), the close loop (20 attempts, fatal), and the checksum verify (20 attempts, fatal) precisely as cited above — this is the family's own distinct retry shape, different from both Task 1 (single-shot, tolerant-close) and Task 3 (single-shot erase, content-blind chunk loop, single-shot close/checksum).

- [ ] **Step 9: Add remaining executor tests one at a time; run the suite after each; add BUILD.bazel targets; verify full PASS.**

- [ ] **Step 10: Wire the desktop workflow** — `SubaruTcuCvtMitsuMh8111Can` route + workflow class.

- [ ] **Step 11: Shrink the drain ratchet** — remove `"tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.cpp"`.

- [ ] **Step 12: Delete the legacy family; flip the matrix row**

`notes` gains: "Write targets block 3 (`{0x80000, 0x100000}`), the family's declared capacity's upper block; read is hardcoded to `{0x8000, 0x78000}` and does not overlap the write region — a genuine legacy asymmetry (read is an incomplete diagnostic dump, write assumes a full `0x180000`-byte user-supplied ROM image), preserved rather than reconciled. No hardware qualification is claimed."

- [ ] **Step 13: Run the full gate; commit.**

Expected: `//:legacy_flash_drain`'s `REMAINING` count is now 19.

---

### Task 5: `FlashTcuCvtSubaruMitsuMH8104Can` — types, plan, executor (tolerant-checks divergence)

**Files:** same five-file-plus-wiring shape, named `subaru_tcu_cvt_mitsu_mh8104_can_*`.

**Interfaces:**
- Produces: `SubaruTcuCvtMitsuMh8104CanPlan`, `build_subaru_tcu_cvt_mitsu_mh8104_can_plan(...)`, `validate_subaru_tcu_cvt_mitsu_mh8104_can_plan(...)`, `class SubaruTcuCvtMitsuMh8104CanExecutor final : public IFlashExecutor`.

- [ ] **Step 1: Re-read `flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.cpp` in full (972 lines) before writing code.** Every one of its response checks after `connect_bootloader_subaru_tcu_mitsu_can`'s kernel-alive probe is followed by a commented-out `// return STATUS_ERROR;` — this family **never fails** from a bad ECU response after that point; only a transport-level error (empty reply causing an out-of-bounds `.at()` read — see Step 8) can stop it. This is the family's defining, deliberate-to-preserve quirk.

- [ ] **Step 2: Write `subaru_tcu_cvt_mitsu_mh8104_can_types.h`**

```cpp
#pragma once
#include <cstdint>

namespace fastecu::flash
{
struct SubaruTcuCvtMitsuMh8104CanPlan
{
    std::uint32_t request_id;  // 0x7e1
    std::uint32_t response_id; // 0x7e9
    int bitrate;               // 500000
    bool extended_id;          // false
};
} // namespace fastecu::flash
```

- [ ] **Step 3: Write the failing plan test** — mirrors Task 1 Step 3, with `kMcu = "MH8104"`, read/write region both `{0x8000, 0x78000}` (unlike MH8111, MH8104's write block 3 **does** coincide with the read window: `fblocks_MH8104[3] = {0x8000, 0x78000}`), image size `0x80000` for both operations (MH8104's true capacity, unlike MH8111's `0x180000`).

- [ ] **Step 4: Run — verify fails to compile.**

- [ ] **Step 5: Write `subaru_tcu_cvt_mitsu_mh8104_can_plan.h`/`.cpp`**

Same shape, `kMcu = "MH8104"`, geometry check `romsize=0x80000, numblocks=4, fblocks[3]={0x8000,0x78000}`, `kReadRegion = kWriteRegion = {0x8000, 0x78000}`, `family_plan = SubaruTcuCvtMitsuMh8104CanPlan{0x7e1, 0x7e9, 500000, false}`.

- [ ] **Step 6: Wire `flash_types.h`**, add BUILD.bazel targets, run plan test, verify PASS, commit.

```bash
git commit -m "feat: portable Subaru TCU CVT Mitsu MH8104 CAN plan (step 5 tail, wave 3)"
```

- [ ] **Step 7: Write the failing executor test**

Connect sequence (legacy lines 89-344): kernel-alive probe `0x31 0x02 0x02 0x01` — **but note: legacy line 121 indexes `received.at(4)` through `.at(7)` with NO length guard before it** (unlike every other family's connect probe, which checks `received.length() > N` first). A scripted empty/short reply at this exact exchange must be handled as a defined `Result` failure by `UdsClient`/the channel before the executor ever indexes into the payload — confirm `uds::payload()`/`uds::subfunction()` (used throughout Task 1/3/4) bounds-check rather than raw-index, and use those helpers here too rather than replicating the legacy raw `.at()` calls. This is the executor being more defensive than the legacy code by construction (via the existing `uds::payload`/`uds::subfunction` helpers), not a new behavior decision — call this out in a one-line comment at the call site, since it is the one place in this family where the port is *structurally* incapable of reproducing legacy's unchecked-index UB, not a deliberate divergence requiring a matrix note.

On alive-probe miss: init retry loop for `0xAA` up to 6 attempts stopping on ANY non-empty reply regardless of content (lines 139-157), then `0x09 0x04` similarly (lines 174-197), then session `0x10 0x43` (non-fatal on mismatch, **no early return at all**, lines 205-226), seed `0x27 0x01` (non-fatal, lines 228-248), seed key `0x27 0x02` (non-fatal, lines 260-282), jump `0x10 0x42` (non-fatal, lines 287-307), then the alive re-check `0x34 0x04 0x33 00 00 00 08 00 00` whose legacy condition uses `&&` instead of `||` across all four byte comparisons (line 334) — **transcribe the bug literally**: `kernel_alive` is set true (and logged as "Kernel verified to be running") only when the response's SID, format id, address-format id, AND length-format id ALL simultaneously differ from the expected `0x74 0x20 0x01 0x04` — i.e. in practice this branch almost never triggers for a well-formed response, and `connect_bootloader_subaru_tcu_mitsu_can` **always returns `STATUS_SUCCESS`** regardless (line 343, unconditional). Port this precisely: the connect function returns success unconditionally after every exchange completes (successfully or not, content-wise) — only a transport-level failure between exchanges (timeout/disconnect/cancellation) stops it.

Write these tests, one at a time:

1. `RejectsAPlanFromAnotherFamilyBeforeAnyIo`
2. `ConnectSkipsTheRestWhenKernelAlreadyRunning` — alive probe returns `0x71 0x02 0x02 0x03`, assert success with zero further writes.
3. `ConnectSucceedsEvenWhenEveryDiagnosticResponseIsWrong` — script every exchange after the alive-probe miss with a deliberately wrong/negative response (never a transport error), assert `execute()` still proceeds past `connect_bootloader` into the read/write phase — this is the test that pins the "commented-out checks" quirk precisely.
4. `ConnectPropagatesATimeoutBetweenExchanges` — a genuine transport-level timeout (empty scripted frame) at, e.g., the seed-key exchange DOES stop the executor (distinguishing "ECU said no" — tolerated — from "nothing came back at all" — still fatal, since there's no reply to even not-check).
5. `ReadReturnsTheWindowPaddedWithFF` — dump sweep over `{0x8000, 0x78000}`, decrypt with `kDecryptTable = {0x6587, 0x4492, 0xa8b4, 0x7bf2}` (same table as MH8111 — confirmed identical in Task 1's Global Constraints), `0xFF` padding.
6. `ReadStopsWhenCancelled`
7. `WriteFlashesTheBlockToleratingEveryContentMismatch` — erase `0x31 0x01 0x02 0x01 0x0f 0xff 0xff 0xff` (a single send with an 8-second-equivalent-in-`FakeClock` delay per legacy lines 875-883 — do not literally sleep in the test; assert the `IClock`/policy timing parameter instead), non-fatal regardless of response content (legacy line 885 logs but never returns error); `reflash_block` setup `0x34 0x04 0x33 <addr3> <len3>` up to 6 attempts stopping on any non-empty reply (non-fatal even then, line 712-715); `maxblocks = 0x78000/128 = 3840` chunks of `0xB6 <addr3> <128 bytes>` with no response check; close `0x37` up to 6 attempts, non-fatal (lines 787-791); checksum `0x31 0x01 0x02 0x02 0x01` up to 6 attempts, non-fatal (lines 828-832); **the executor still returns `FlashExecutionResult` success at the end** because nothing in this family's write path ever returns an error from a bad/negative ECU response — only a genuine transport failure (timeout/disconnect) between sends does.
8. `WriteStopsOnATimeoutBetweenChunks`
9. `WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo`
10. `RefusesATestWritePlanRatherThanWritingForReal`

- [ ] **Step 8: Write `subaru_tcu_cvt_mitsu_mh8104_can_executor.h`/`.cpp`**

Crypto tables (legacy lines 899-965, identical to MH8111's — confirm by direct comparison before writing, do not assume):

```cpp
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
    0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x7bf2, 0xa8b4, 0x4492, 0x6587};
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x6587, 0x4492, 0xa8b4, 0x7bf2};
```

The defining implementation shape: `connect_bootloader` never returns `Error{...}` for a content mismatch, only for a `UdsClient::request()` failure (timeout/disconnect/cancellation) — every "check" from legacy becomes an `if (!content_ok) { info(ctx, "..."); }` with **no early return**, exactly mirroring the commented-out `// return STATUS_ERROR;` lines. The retry loops (6 attempts for `0xAA`, `0x09 0x04`, `reflash_block` setup, close, checksum) stop as soon as ANY reply arrives (content unchecked) — model this as: call `ctx.uds.request(...)`, and only treat `!received.has_value()` as needing a retry, up to the attempt cap, after which proceed regardless (do not fail even after exhausting attempts, since legacy's `try_count == N` branches here also only `LOG_I`, they do not `return STATUS_ERROR` — **re-verify this precisely against lines 694-715, 770-791, and 811-832 before implementing**, since it is easy to misread the erase function's stricter cousin in Task 4 as this family's shape by mistake).

- [ ] **Step 9: Add remaining executor tests one at a time; run the suite after each; add BUILD.bazel targets; verify full PASS.**

- [ ] **Step 10: Wire the desktop workflow** — `SubaruTcuCvtMitsuMh8104Can` route + workflow class.

- [ ] **Step 11: Shrink the drain ratchet** — remove `"tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.cpp"`. `REMAINING` is now empty of every wave-3 family.

- [ ] **Step 12: Delete the legacy family; flip the matrix row**

`notes` gains: "Every response check after the kernel-alive probe is commented out in legacy (`// return STATUS_ERROR;`) — this family tolerates any ECU response content and only a transport-level failure (timeout/disconnect/cancellation) stops it; preserved exactly, including the connect alive-recheck's `&&`-instead-of-`||` condition (line 334) which means that branch practically never fires for a well-formed response. No hardware qualification is claimed."

- [ ] **Step 13: Run the full gate; commit.**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
git add -A
git commit -m "feat: portable Subaru TCU CVT Mitsu MH8104 CAN executor + workflow (step 5 tail, wave 3)"
```

Expected: `//:legacy_flash_drain`'s `REMAINING` count is now 18 (down from 22 at wave 2's close).

---

### Task 6: Cluster factoring — MH8111/MH8104 common core

**Files:**
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h`
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.cpp`
- Create: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: `SubaruTcuCvtMitsuMh8111CanExecutor`'s and `SubaruTcuCvtMitsuMh8104CanExecutor`'s already-tested implementations from Tasks 4-5 (the executor tests written there are the regression guard for this task — do not change their expected wire bytes).
- Produces: whatever shared helper(s) survive comparison — named here only provisionally; this task's own investigation may conclude with **no new file** (see Step 1).

- [ ] **Step 1: Compare the two already-merged, already-tested executors line by line — `connect_bootloader`, the seed/key derivation call shape, `erase`, `reflash_block`'s setup+chunk-loop+close, and the checksum verify.**

Per the design's stated hypothesis, the two are near-identical apart from write-chunk size (256 for MH8111, 128 for MH8104) and content-check strictness (MH8111 fails after N attempts on several steps; MH8104 never fails from content, only from a transport error). **The chunk-size and strictness differences are real behavioral differences, not incidental** — any shared helper must take both as explicit parameters rather than assuming one family's constants, or it recreates exactly the "configurable state machine" the tail design forbids extracting. If, after this comparison, no helper can be extracted without becoming a policy-parameterized dispatcher, stop here: this task produces no new file, and Step 2 onward do not apply. Record the finding either way in the PR description.

- [ ] **Step 2 (if a genuine common core exists): Identify the literal, byte-identical shared pieces only**

Candidates supported by Task 4/5's confirmed findings: the seed-key table (`kSeedKeyTable`, both families, byte-identical), the encrypt/decrypt tables (byte-identical), the shared `kIndexTransformation` table (already shared with every other family in this wave — do not re-factor this one, it stays a per-file constant matching every sibling executor's own copy, per this project's established pattern of one copy per file rather than a shared crypto-constants header), and the block-modified skip logic (`blocks 0-2 skipped, block 3 flashed` — identical shape, different absolute addresses).

- [ ] **Step 3: If a helper is justified, write it with both families' policy explicit**

Example shape (only if Step 1 confirms it):

```cpp
// subaru_tcu_cvt_mitsu_can_common.h
#pragma once
#include <cstdint>
#include "src/algorithms/protocol/bytes.h"

namespace fastecu::flash
{
// Shared MH8111/MH8104 seed-key table -- confirmed byte-identical in both
// legacy generate_seed_key implementations (mh8111 lines 908-913, mh8104
// lines 903-908).
extern const std::array<std::uint16_t, 16>& tcuCvtMitsuSeedKeyTable();
} // namespace fastecu::flash
```

Do not extract `reflash_block`'s chunk loop itself unless its two call sites, after Task 4/5, are byte-identical modulo only the chunk-size constant and content-check policy — if extraction requires branching on a "strictness" enum inside the shared function, that is the forbidden configurable state machine; leave `reflash_block` un-factored in each executor and extract only the crypto table.

- [ ] **Step 4: Update both executors to call the shared helper; run both executor test suites unchanged — they must still pass with zero test edits**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8111_can_executor_test //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8104_can_executor_test`
Expected: PASS, unchanged assertions — this is the proof the factoring changed no behavior.

- [ ] **Step 5: Run the full gate; commit**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
git add -A
git commit -m "refactor: factor the MH8111/MH8104 seed-key table (step 5 tail, wave 3 close)"
```

(If Step 1 found no extractable core, commit nothing for this task beyond a note in the wave's closing PR description — the wave still completes, per the tail design's explicit allowance.)

---

### Task 7: Wave close — modularization plan update

**Files:**
- Modify: `docs/modularization-plan.md`

- [ ] **Step 1: Update the plan's Status section**

Add a line after the existing "Wave 2 ... merged" bullet: "Wave 3 `FlashEcuSubaruHitachiM32rCan`, `FlashTcuCvtSubaruHitachiM32rCan`, `FlashTcuCvtSubaruMitsuMH8111Can`, `FlashTcuCvtSubaruMitsuMH8104Can` — merged. Takes the drain from 22 remaining families to 18." Update the "Make backend workflows portable" roadmap item's parenthetical family count to match.

- [ ] **Step 2: Verify the drain ratchet count matches the doc update**

Run: `python3 -c "import sys; sys.path.insert(0, 'scripts'); import importlib; m = importlib.import_module('check-legacy-flash-drain'); print(len(m.REMAINING))"`
Expected: `18`.

- [ ] **Step 3: Commit**

```bash
git add docs/modularization-plan.md
git commit -m "docs: record wave-3 completion in the modularization plan"
```

---

## Self-review notes (carried in this document, not a separate step)

- **Spec coverage:** every section of the wave-3 design doc has a task — scope/families (Tasks 1, 3-5), portable contract (Global Constraints + per-task exchange citations), UDS-layer reuse (no new port surface introduced anywhere in this plan), cluster factoring (Task 6, with the explicit "may produce nothing" escape hatch preserved), testing (per-task test lists), sequence (Tasks 1-7 mirror the spec's PR sequence exactly), matrix updates (每 family task's final steps).
- **Two open numeric findings surfaced only during this plan's research, beyond what the approved design doc states**, both resolved here rather than left as TBDs: (1) `SubaruTcuCvtHitachiM32rCan`'s address-bias underflow (Task 3's Global Constraints entry and Step 3/16), (2) `SubaruTcuCvtMitsuMh8111Can`'s read/write region non-overlap (Task 4's Step 3/12). Both are called out in their task's matrix-note step so they reach the standing ledger, per the umbrella's disclosure rule.
- **Type/signature consistency:** every family's plan struct, builder function name, validator function name, and executor class name follow the exact `<snake_case_family>` / `<PascalCase_family>` pairing used by every existing sibling (`subaru_hitachi_m32r_kline_plan.h` / `SubaruHitachiM32rKlinePlan`), checked across Tasks 1, 3, 4, 5's Interfaces blocks for cross-task consistency.
