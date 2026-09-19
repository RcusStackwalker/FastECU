<!-- docs/superpowers/plans/2026-09-19-step5-tail-wave6a1-tcu-hitachi-m32r-kline.md -->
# Wave 6a-1 — Subaru TCU Hitachi M32R K-Line — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate `FlashTcuSubaruHitachiM32rKlineOperation` (733 lines, legacy Qt) to a portable `FlashPlan` + `IKlineFlashExecutor` pair, removing one entry from `//:legacy_flash_drain`.

**Architecture:** The family is read-only SSM-over-K-Line at 4800 baud with ISO-14230 connection framing: a five-exchange authenticated connect (`0xBF` ECU ID, `0x81` start communication, `0x83 0x00` timing parameters, `0x27 0x01` seed, `0x27 0x02` key) followed by a loop of `0xA0` block reads of 96 bytes each. Plan validation reuses the shared `single_window_plan` helper; the executor is hand-written and un-factored, matching every other family in the package.

**Tech Stack:** C++23, Bazel, GoogleTest (`fastecu_portable_gtest`), QtTest for the desktop workflow suite. Portable backend code: no Qt, no threads, no filesystem.

**Spec:** [Step 5 Tail Wave 6 — Nine Singletons](../specs/2026-09-19-step5-tail-wave6-singletons-design.md)

## Global Constraints

- Backend operations return `fastecu::Result<T>`, checked with `.has_value()` and **never** the implicit `operator bool`.
- **Exceptions never cross a port.** The `ErrorKind` set is closed — do not add a value.
- Pure protocol logic uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView`. `QByteArray` is a boundary type only.
- New backend targets are portable: registered in `PORTABLE_PACKAGES` already (`//src/backend/flash/ecu` is in the portable closure); no `//src/platform` label may become reachable from them.
- **No entry may be added to any ratchet list** (`serial_qt_compat` allowlist, `REMAINING` in `scripts/check-legacy-flash-drain.py`). Needing one means the change took the legacy path.
- Tests are package-owned and co-located: `foo.cpp` + `foo_test.cpp` in the same package.
- Work lands through a pull request; `prek` refuses commits on `master`. Branch before the first commit.
- Every commit message ends with the two attribution lines used by this repo.

## Deliberate Divergences From Legacy

These are behavior changes, made knowingly, each with a reason. They belong in the PR description. Everything not listed here is byte-preserving.

1. **Write and test_write are rejected at plan build.** The legacy `execute()`'s `"test_write"`/`"write"` branch logs `"Not yet implemented: Writing ROM to TCU Subaru Hitachi using CAN"` and then returns `result_arg`, which still holds `connect_bootloader()`'s `STATUS_SUCCESS` — so the dialog reports a **successful write that wrote nothing**. Preserving that is not an option; the portable plan builder returns `Unsupported`, and the matrix row records `operations = read`.
2. **A block read that fails all five attempts is an error, not a silent gap.** The legacy loop retries five times, then appends nothing and continues, returning `STATUS_SUCCESS` at the end — producing a short, silently misaligned ROM. The portable executor returns `BadResponse`.
3. **Block-read responses are length-checked exactly.** The legacy accepted any response longer than 5 bytes and appended `length - 1` of it, so a short response misaligned every subsequent byte. The executor requires exactly `block_len + 6` bytes, matching the equivalent check in the already-migrated `SubaruHitachiM32rKlineExecutor`.
4. **Dead code is not ported.** `read_b8`, `read_b0`, `read_a0_ram`, `send_sid_b0_block_write`, `send_sid_b8_byte_read` and `generate_can_seed_key` are unreachable — their only call sites are commented out in `execute()` and `read_a0_rom`. Wave 3 set this precedent with `hack_words()`.

**One value this repository cannot supply:** the MCU name the `sub_tcu_hitachi_m32r_kline` protocol ships with. Protocol-to-MCU mapping lives in the EcuFlash-side definition files, not here; the only in-repo evidence is that the wave-3 TCU family (`subaru_tcu_cvt_hitachi_m32r_can_plan.cpp:49`) uses `M32R_512KB`. This plan uses `M32R_512KB`. Flag it as a `VERIFY` line in the PR description. The blast radius is bounded: the family is read-only, and `geometry_ok` fails loudly on a mismatch rather than reading the wrong window.

## File Structure

**Create (portable backend, `src/backend/flash/ecu/`):**
- `subaru_tcu_hitachi_m32r_kline_types.h` — the plan POD. One responsibility: wire parameters.
- `subaru_tcu_hitachi_m32r_kline_plan.{h,cpp}` — build and validate. No I/O.
- `subaru_tcu_hitachi_m32r_kline_plan_test.cpp`
- `subaru_tcu_hitachi_m32r_kline_executor.{h,cpp}` — the `IKlineFlashExecutor`.
- `subaru_tcu_hitachi_m32r_kline_executor_test.cpp`

**Modify:**
- `src/backend/flash/flash_types.h` — `FlashFamily` value, `FamilyPlan` alternative, `FamilyTraits` specialization, `family_requires_kernel_v` specialization.
- `src/backend/flash/ecu/single_window_plan.{h,cpp}` — add `supports_write`.
- `src/backend/flash/ecu/single_window_plan_test.cpp` — cover the new flag.
- `src/backend/flash/ecu/BUILD.bazel` — three new `cc_library` targets, two new test targets.
- `src/platform/desktop/common/flash/flash_workflow.cpp` — `Route::Kind`, `kRoutes`, factory switch, workflow class.
- `src/platform/desktop/common/flash/flash_workflow_test.cpp` — routing coverage.
- `src/ui/desktop/mainwindow.cpp:1343-1347` — delete the legacy branch.
- `scripts/check-legacy-flash-drain.py` — remove one `REMAINING` entry.
- `docs/flash-qualification-matrix.md` — flip the row.

**Delete:**
- `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.{h,cpp}`
- `src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_kline.{h,cpp}`

---

### Task 1: Plan builder, types, and the read-only flag

**Files:**
- Create: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_types.h`
- Create: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h`, `…_plan.cpp`
- Create: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan_test.cpp`
- Modify: `src/backend/flash/flash_types.h`, `src/backend/flash/ecu/single_window_plan.h`, `single_window_plan.cpp`, `single_window_plan_test.cpp`, `src/backend/flash/ecu/BUILD.bazel`
- Test: `//src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_plan_test`, `//src/backend/flash/ecu:single_window_plan_test`

**Interfaces:**
- Consumes: `SingleWindowPlanSpec`, `build_single_window_plan`, `validate_single_window_plan` from `single_window_plan.h`; `FlashPlan`, `FlashPlanFields`, `validate_and_build`.
- Produces: `struct SubaruTcuHitachiM32rKlinePlan{std::uint8_t tester_id; std::uint8_t target_id; int baud; std::uint32_t block_size;}`; `Result<FlashPlan> build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation, std::string_view protocol_name, std::string_view mcu_type, std::optional<bytes::Bytes> image)`; `Status validate_subaru_tcu_hitachi_m32r_kline_plan(const FlashPlan&)`; `FlashFamily::SubaruTcuHitachiM32rKline`; `SingleWindowPlanSpec::supports_write`.

- [ ] **Step 1: Write the failing plan test**

Create `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kReadCase{
    .name = "SubaruTcuHitachiM32rKline",
    .build = &build_subaru_tcu_hitachi_m32r_kline_plan,
    .protocol = "sub_tcu_hitachi_m32r_kline",
    .mcu = "M32R_512KB",
    .foreign_protocol = "sub_tcu_hitachi_m32r_kline_typo",
    .foreign_mcu = "M32R_512KB_1block",
    .read_region = MemoryRegion{.start = 0, .length = 0x80000},
    .erase_region = MemoryRegion{.start = 0, .length = 0x80000},
    .image_size = 0x80000,
};

INSTANTIATE_TEST_SUITE_P(SubaruTcuHitachiM32rKline, SingleWindowPlanContract, ::testing::Values(kReadCase), caseName);

TEST(SubaruTcuHitachiM32rKlinePlan, MapsProtocolToItsWireParameters)
{
    const auto plan =
        build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_tcu_hitachi_m32r_kline", "M32R_512KB",
                                                 std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->family(), FlashFamily::SubaruTcuHitachiM32rKline);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);

    const auto& family = std::get<SubaruTcuHitachiM32rKlinePlan>(plan->family_plan());
    EXPECT_EQ(family.tester_id, 0xf0);
    EXPECT_EQ(family.target_id, 0x18);
    EXPECT_EQ(family.baud, 4800);
    EXPECT_EQ(family.block_size, 96u);
}

// Deliberate divergence 1: the legacy write branch reported success having
// written nothing. See the plan's "Deliberate Divergences From Legacy".
TEST(SubaruTcuHitachiM32rKlinePlan, RejectsWriteAndTestWriteAsUnsupported)
{
    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        const auto plan = build_subaru_tcu_hitachi_m32r_kline_plan(operation, "sub_tcu_hitachi_m32r_kline",
                                                                   "M32R_512KB", bytes::Bytes(0x80000, 0x00));
        EXPECT_THAT(plan, fastecu::testing::IsErrorOfKind(ErrorKind::Unsupported));
    }
}
} // namespace
} // namespace fastecu::flash::testing
```

Add to `single_window_plan_test.cpp`, proving the flag does not disturb the ten existing families:

```cpp
TEST(SingleWindowPlan, SupportsWriteDefaultsToTrueAndGatesWriteWhenFalse)
{
    EXPECT_TRUE(SingleWindowPlanSpec{}.supports_write);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_plan_test`
Expected: FAIL at analysis — `no such target … :subaru_tcu_hitachi_m32r_kline_plan_test`.

- [ ] **Step 3: Add the family to the type system**

In `src/backend/flash/flash_types.h`, add the include next to its siblings, the enum value at the end of `FlashFamily` under a `// Step 5 tail, wave 6a.` comment, the alternative at the end of `FamilyPlan`, and:

```cpp
template <> struct FamilyTraits<SubaruTcuHitachiM32rKlinePlan>
{
    static constexpr FlashFamily family = FlashFamily::SubaruTcuHitachiM32rKline;
    static constexpr TransportKind transport = TransportKind::Kline;
};

// Step 5 tail, wave 6a. Authenticates against the TCU's resident bootloader
// via SecurityAccess and reads with 0xA0 block reads, uploading no image.
template <> inline constexpr bool family_requires_kernel_v<SubaruTcuHitachiM32rKlinePlan> = false;
```

- [ ] **Step 4: Add the read-only flag to the shared spec**

In `single_window_plan.h`, append to `SingleWindowPlanSpec`:

```cpp
    // False for families whose legacy class never implemented a write path.
    // Defaults true, so the ten families that predate this flag are
    // unchanged.
    bool supports_write = true;
```

In `single_window_plan.cpp`, add the same guard to both entry points. In `validate_single_window_plan`, immediately after the existing `TestWrite` rejection:

```cpp
    if (!spec.supports_write && plan.operation() == FlashOperation::Write)
    {
        return fail(Unsupported, std::format("write is not supported by {}", spec.display_name));
    }
```

and in `build_single_window_plan`, immediately after its existing `TestWrite` rejection:

```cpp
    if (!spec.supports_write && operation == FlashOperation::Write)
    {
        return fail(Unsupported, std::format("write is not supported by {}", spec.display_name));
    }
```

- [ ] **Step 5: Write the types header**

Create `subaru_tcu_hitachi_m32r_kline_types.h`:

```cpp
#pragma once
#include <cstdint>

namespace fastecu::flash
{
// Legacy: flash_tcu_subaru_hitachi_m32r_kline_operation.{h,cpp}, read path
// only. Its execute() "test_write"/"write" branch logged "Not yet
// implemented" and then returned connect_bootloader()'s STATUS_SUCCESS,
// reporting a successful write that wrote nothing; this plan rejects both
// operations instead. See the wave 6a-1 plan's "Deliberate Divergences".
struct SubaruTcuHitachiM32rKlinePlan
{
    std::uint8_t tester_id;   // 0xf0
    std::uint8_t target_id;   // 0x18, not the ECU family's 0x10
    int baud;                 // 4800, never changed mid-session
    std::uint32_t block_size; // 96, not the ECU family's 128
};
} // namespace fastecu::flash
```

- [ ] **Step 6: Write the plan builder**

Create `subaru_tcu_hitachi_m32r_kline_plan.h`:

```cpp
#pragma once

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type,
                                                           std::optional<bytes::Bytes> image);

Status validate_subaru_tcu_hitachi_m32r_kline_plan(const FlashPlan& plan);
} // namespace fastecu::flash
```

Create `subaru_tcu_hitachi_m32r_kline_plan.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::string_view kProtocol = "sub_tcu_hitachi_m32r_kline";
constexpr std::array kProtocols{kProtocol};

constexpr MemoryRegion kRom{0, 0x80000};

bool geometry_ok(const flashdev_t& device)
{
    return device.romsize == kRom.length && device.fblocks[0].start == kRom.start;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruTcuHitachiM32rKlinePlan>(&plan.family_plan());
    return p != nullptr && p->tester_id == 0xf0 && p->target_id == 0x18 && p->baud == 4800 && p->block_size == 96;
}

// write_region and image_size are unreachable for this family -- supports_write
// is false -- but the shared spec requires both; they mirror read_region so a
// future write path would not inherit a wrong window by default.
constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru TCU Hitachi M32R K-Line",
    .protocols = kProtocols,
    .mcu = "M32R_512KB",
    .family = FlashFamily::SubaruTcuHitachiM32rKline,
    .transport = TransportKind::Kline,
    .read_region = kRom,
    .write_region = kRom,
    .image_size = kRom.length,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
    .supports_write = false,
};
} // namespace

Status validate_subaru_tcu_hitachi_m32r_kline_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruTcuHitachiM32rKlinePlan{0xf0, 0x18, 4800, 96});
}
} // namespace fastecu::flash
```

- [ ] **Step 7: Add the Bazel targets**

In `src/backend/flash/ecu/BUILD.bazel`, mirroring the `subaru_hitachi_m32r_kline` block:

```python
cc_library(
    name = "subaru_tcu_hitachi_m32r_kline_types",
    hdrs = ["subaru_tcu_hitachi_m32r_kline_types.h"],
)

cc_library(
    name = "subaru_tcu_hitachi_m32r_kline_plan",
    srcs = ["subaru_tcu_hitachi_m32r_kline_plan.cpp"],
    hdrs = ["subaru_tcu_hitachi_m32r_kline_plan.h"],
    deps = [
        ":single_window_plan",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/flash:flash_plan",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports",
    ],
)
```

Add `":subaru_tcu_hitachi_m32r_kline_types"` to the `deps` of the `flash_types`-bearing target that lists the other `*_types` libraries, and register the test with `fastecu_portable_gtest`, copying the `subaru_hitachi_m32r_kline_plan_test` target's shape.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_plan_test //src/backend/flash/ecu:single_window_plan_test`
Expected: PASS, both targets.

- [ ] **Step 9: Verify no existing family changed behavior**

Run: `bazel test --config=release //src/backend/flash/...`
Expected: PASS. The `supports_write` default of `true` means the ten pre-existing single-window families are untouched; a failure here means the flag was wired in the wrong place.

- [ ] **Step 10: Commit**

```bash
git checkout -b flash/wave6a1-tcu-hitachi-m32r-kline
git add src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_types.h \
        src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h \
        src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.cpp \
        src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan_test.cpp \
        src/backend/flash/ecu/single_window_plan.h \
        src/backend/flash/ecu/single_window_plan.cpp \
        src/backend/flash/ecu/single_window_plan_test.cpp \
        src/backend/flash/ecu/BUILD.bazel \
        src/backend/flash/flash_types.h
git commit -m "feat(flash): add the portable TCU Hitachi M32R K-Line plan"
```

---

### Task 2: Executor transport setup and the authenticated connect

**Files:**
- Create: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h`, `…_executor.cpp`
- Create: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Test: `//src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test`

**Interfaces:**
- Consumes: Task 1's `build_…_plan` / `validate_…_plan` / `SubaruTcuHitachiM32rKlinePlan`; `IKlineFlashExecutor`, `IKlineFlashTransport`, `KlineConfig`, `check_family` from `flash_executor.h`; `SsmProtocol::addHeader`, `SsmProtocol::calculateSeedKey`, `SsmProtocol::kIndexTransformationStock`.
- Produces: `class SubaruTcuHitachiM32rKlineExecutor final : public IKlineFlashExecutor` with `transport_setup(const FlashPlan&) const` and `execute(const FlashPlan&, IKlineFlashTransport&, IClock&, const ICancellationToken&, IEventSink&)`.

The legacy setup is `set_is_can_connection(false)`, `set_is_iso15765_connection(false)`, **`set_is_iso14230_connection(true)`**, `open_serial_port()`, `change_port_speed("4800")`, `set_add_iso14230_header(false)`. Note `iso14230 = true` here — the already-migrated ECU sibling uses `non_iso14230_kline_config_from()`, which would be wrong for this family.

- [ ] **Step 1: Write the failing test for transport setup and the connect sequence**

Create `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor_test.cpp`:

```cpp
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h"

#include <gtest/gtest.h>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using namespace fastecu;
using namespace fastecu::flash;

constexpr std::uint32_t kRomSize = 0x80000;
constexpr std::uint32_t kBlockSize = 96;

bytes::Bytes frame(bytes::Bytes payload)
{
    return SsmProtocol::addHeader(payload, 0xf0, 0x18);
}

// 5 header bytes (0x80, tester, target, length, response code), then data,
// then one checksum byte -- the shape read_a0_rom slices.
bytes::Bytes blockResponse(std::uint32_t length, bytes::Byte fill)
{
    bytes::Bytes response(length + 6, fill);
    response[4] = 0xe0;
    return response;
}

bytes::Bytes idResponse()
{
    return {0x80, 0xf0, 0x18, 0x09, 0xff, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0};
}

bytes::Bytes seedResponse()
{
    return {0x80, 0xf0, 0x18, 0x06, 0x67, 0x01, 0xde, 0xad, 0xbe, 0xef, 0};
}

bytes::Bytes expectedSeedKey()
{
    static constexpr std::array<std::uint16_t, 16> index = {0x0FE9, 0xCA58, 0x5E90, 0xDFF1, 0x690B, 0xF591,
                                                            0x1794, 0x5C7B, 0xA7BF, 0x98E5, 0x0B63, 0xA1C9,
                                                            0x79BF, 0xF413, 0x82B1, 0xA895};
    const bytes::Bytes seed{0xde, 0xad, 0xbe, 0xef};
    return SsmProtocol::calculateSeedKey(seed, index, SsmProtocol::kIndexTransformationStock);
}

void scriptConnect(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.section("connect");
    transport.exchange(frame({0xbf}), idResponse());
    transport.exchange(frame({0x81}), {0x80, 0xf0, 0x18, 0x01, 0xc1, 0});
    transport.exchange(frame({0x83, 0x00}), {0x80, 0xf0, 0x18, 0x01, 0xc3, 0});
    transport.exchange(frame({0x27, 0x01}), seedResponse());
    bytes::Bytes key_request{0x27, 0x02};
    const bytes::Bytes key = expectedSeedKey();
    key_request.insert(key_request.end(), key.begin(), key.end());
    transport.exchange(frame(key_request), {0x80, 0xf0, 0x18, 0x02, 0x67, 0x02, 0});
}

FlashPlan readPlan()
{
    auto plan = build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_tcu_hitachi_m32r_kline",
                                                        "M32R_512KB", std::nullopt);
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

TEST(SubaruTcuHitachiM32rKlineExecutor, TransportSetupMatchesTheLegacySetters)
{
    SubaruTcuHitachiM32rKlineExecutor executor;
    const auto config = executor.transport_setup(readPlan());
    ASSERT_THAT(config, fastecu::testing::IsOk());
    EXPECT_EQ(config->baud, 4800);
    // set_is_iso14230_connection(true) in the legacy execute().
    EXPECT_TRUE(config->iso14230);
    EXPECT_EQ(config->tester_id, 0xf0);
    EXPECT_EQ(config->target_id, 0x18);
}

TEST(SubaruTcuHitachiM32rKlineExecutor, ConnectSendsTheFiveLegacyExchangesInOrder)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    for (std::uint32_t address = 0; address < kRomSize; address += kBlockSize)
    {
        const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
        transport.exchange(frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16),
                                  static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address),
                                  static_cast<bytes::Byte>(length - 1)}),
                           blockResponse(length, 0x5a));
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    ManualCancellationToken cancellation;
    fastecu::testing::RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result->rom_id, "123456789A_");
}
} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test`
Expected: FAIL at analysis — the target and the executor header do not exist.

- [ ] **Step 3: Write the executor header**

Create `subaru_tcu_hitachi_m32r_kline_executor.h`:

```cpp
#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruTcuHitachiM32rKlineExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
```

- [ ] **Step 4: Write the executor's setup and connect**

Create `subaru_tcu_hitachi_m32r_kline_executor.cpp`. This step writes everything except `read_rom`, which is Task 3; `execute()` calls it, so add a stub returning `fail(ErrorKind::Internal, "not implemented")` and replace it in Task 3.

```cpp
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h"

#include <array>
#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using namespace bytes::literals;
using namespace std::chrono_literals;

constexpr std::uint32_t kRomSize = 0x80000;
// Legacy serial_read_timeout, used by every connect_bootloader() exchange.
constexpr int kConnectTimeoutMs = 2000;
// Legacy receive_timeout, used by send_sid_a0_block_read().
constexpr int kBlockTimeoutMs = 500;
// The delay(100) on each side of send_sid_a0_block_read()'s read.
constexpr auto kBlockDelay = 100ms;
// read_a0_rom retries a block up to five times before giving up.
constexpr int kBlockAttempts = 5;

bytes::Bytes framed(bytes::ByteView payload, const SubaruTcuHitachiM32rKlinePlan& p)
{
    return SsmProtocol::addHeader(payload, p.tester_id, p.target_id);
}

Result<std::optional<bytes::Bytes>> exchange_optional(IKlineFlashTransport& transport,
                                                      const ICancellationToken& cancellation, bytes::ByteView payload,
                                                      const SubaruTcuHitachiM32rKlinePlan& p, int timeout)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    const bytes::Bytes request = framed(payload, p);
    auto written = transport.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return fail(ErrorKind::Disconnected, "short K-Line write");
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after write");
    }
    auto response = transport.read(std::chrono::milliseconds{timeout}, cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after read");
    }
    return std::move(*response);
}

Result<bytes::Bytes> exchange(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                              bytes::ByteView payload, const SubaruTcuHitachiM32rKlinePlan& p, int timeout)
{
    auto response = exchange_optional(transport, cancellation, payload, p, timeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from TCU");
    }
    return std::move(**response);
}

Status expect_prefix(bytes::ByteView response, std::initializer_list<bytes::Byte> prefix)
{
    if (response.size() < 4 + prefix.size())
    {
        return fail(ErrorKind::BadResponse, "response is too short");
    }
    std::size_t i = 4;
    for (bytes::Byte value : prefix)
    {
        if (response[i++] != value)
        {
            return fail(ErrorKind::BadResponse, "wrong response from TCU");
        }
    }
    return {};
}

Status request_prefix(IKlineFlashTransport& transport, const ICancellationToken& cancellation, bytes::Bytes request,
                      std::initializer_list<bytes::Byte> expected, const SubaruTcuHitachiM32rKlinePlan& p)
{
    auto response = exchange(transport, cancellation, request, p, kConnectTimeoutMs);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    return expect_prefix(*response, expected);
}

// Legacy: received.remove(0, 8); received.remove(5, ...) -- bytes 8..12 of the
// 0xBF response, hex-encoded, with a trailing underscore.
Result<std::string> parse_rom_id(bytes::ByteView response)
{
    if (auto valid = expect_prefix(response, {0xff}); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (response.size() < 13)
    {
        return fail(ErrorKind::BadResponse, "TCU ID response is too short");
    }
    std::string id;
    for (std::size_t i = 8; i < 13; ++i)
    {
        id += std::format("{:02X}", response[i]);
    }
    return id + '_';
}

// This family's own 16-entry generation table. Its index transformation was
// compared byte for byte against SsmProtocol::kIndexTransformationStock and is
// identical, so only the generation table is family-specific.
bytes::Bytes seed_key(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> index = {0x0FE9, 0xCA58, 0x5E90, 0xDFF1, 0x690B, 0xF591,
                                                            0x1794, 0x5C7B, 0xA7BF, 0x98E5, 0x0B63, 0xA1C9,
                                                            0x79BF, 0xF413, 0x82B1, 0xA895};
    return SsmProtocol::calculateSeedKey(seed, index, SsmProtocol::kIndexTransformationStock);
}

Result<std::string> connect_bootloader(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                       const SubaruTcuHitachiM32rKlinePlan& p)
{
    auto id_response = exchange(transport, cancellation, bytes::Bytes{0xbf}, p, kConnectTimeoutMs);
    if (!id_response.has_value())
    {
        return std::unexpected(id_response.error());
    }
    auto id = parse_rom_id(*id_response);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (auto s = request_prefix(transport, cancellation, {0x81}, {0xc1}, p); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    if (auto s = request_prefix(transport, cancellation, {0x83, 0x00}, {0xc3}, p); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    auto seed =
        exchange(transport, cancellation, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, p,
                 kConnectTimeoutMs);
    if (!seed.has_value())
    {
        return std::unexpected(seed.error());
    }
    if (auto s = expect_prefix(*seed, {0x67, uds::kSecurityAccessRequestSeed}); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    if (seed->size() < 10)
    {
        return fail(ErrorKind::BadResponse, "seed response is too short");
    }
    bytes::Bytes key_request =
        composeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, seed_key(bytes::ByteView{*seed}.subspan(6, 4)));
    if (auto s = request_prefix(transport, cancellation, std::move(key_request),
                                {0x67, uds::kSecurityAccessSendKey}, p);
        !s.has_value())
    {
        return std::unexpected(s.error());
    }
    return id;
}

Result<bytes::Bytes> read_rom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruTcuHitachiM32rKlinePlan& p);
} // namespace

Result<KlineConfig> SubaruTcuHitachiM32rKlineExecutor::transport_setup(const FlashPlan& plan) const
{
    if (const Status match = check_family(plan, FlashFamily::SubaruTcuHitachiM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = validate_subaru_tcu_hitachi_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& p = std::get<SubaruTcuHitachiM32rKlinePlan>(plan.family_plan());
    // Unlike the ECU sibling, this family sets is_iso14230_connection(true),
    // so non_iso14230_kline_config_from() must not be used here.
    return KlineConfig{
        .baud = p.baud,
        .iso14230 = true,
        .tester_id = p.tester_id,
        .target_id = p.target_id,
    };
}

Result<FlashExecutionResult> SubaruTcuHitachiM32rKlineExecutor::execute(const FlashPlan& plan,
                                                                        IKlineFlashTransport& transport, IClock& clock,
                                                                        const ICancellationToken& cancellation,
                                                                        IEventSink& events)
{
    if (const Status match = check_family(plan, FlashFamily::SubaruTcuHitachiM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = validate_subaru_tcu_hitachi_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (plan.operation() != FlashOperation::Read)
    {
        return fail(ErrorKind::Unsupported, "Subaru TCU Hitachi M32R K-Line supports read only");
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }
    const auto& p = std::get<SubaruTcuHitachiM32rKlinePlan>(plan.family_plan());
    if (auto header = transport.set_add_iso14230_header(false); !header.has_value())
    {
        return std::unexpected(header.error());
    }
    auto id = connect_bootloader(transport, cancellation, p);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    auto rom = read_rom(transport, clock, cancellation, events, p);
    if (!rom.has_value())
    {
        return std::unexpected(rom.error());
    }
    return FlashExecutionResult{FlashOperation::Read, std::move(*rom), std::move(*id)};
}
} // namespace fastecu::flash
```

Then add, inside the same anonymous namespace, the temporary stub this task compiles against:

```cpp
Result<bytes::Bytes> read_rom(IKlineFlashTransport&, IClock&, const ICancellationToken&, IEventSink&,
                              const SubaruTcuHitachiM32rKlinePlan&)
{
    return fail(ErrorKind::Internal, "read_rom lands in task 3");
}
```

- [ ] **Step 5: Add the Bazel target**

```python
cc_library(
    name = "subaru_tcu_hitachi_m32r_kline_executor",
    srcs = ["subaru_tcu_hitachi_m32r_kline_executor.cpp"],
    hdrs = ["subaru_tcu_hitachi_m32r_kline_executor.h"],
    deps = [
        ":subaru_tcu_hitachi_m32r_kline_plan",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/ssm",
        "//src/algorithms/protocol/uds:uds_service_ids",
        "//src/backend/flash:flash_executor",
        "//src/backend/ports",
    ],
)
```

Register `subaru_tcu_hitachi_m32r_kline_executor_test` with `fastecu_portable_gtest`, copying the `subaru_hitachi_m32r_kline_executor_test` target's `deps`.

- [ ] **Step 6: Run the tests**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test`
Expected: `TransportSetupMatchesTheLegacySetters` PASSES; `ConnectSendsTheFiveLegacyExchangesInOrder` FAILS with `read_rom lands in task 3`. That split is the point — the connect sequence is proven before the read loop exists.

- [ ] **Step 7: Commit**

```bash
git add src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h \
        src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.cpp \
        src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor_test.cpp \
        src/backend/flash/ecu/BUILD.bazel
git commit -m "feat(flash): add the TCU Hitachi M32R K-Line connect sequence"
```

---

### Task 3: The 0xA0 block-read loop

**Files:**
- Modify: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor_test.cpp`
- Test: `//src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test`

**Interfaces:**
- Consumes: Task 2's `exchange`, `framed`, `kBlockTimeoutMs`, `kBlockDelay`, `kBlockAttempts`, `kRomSize`.
- Produces: the real `read_rom`, replacing Task 2's stub. No new public symbols.

Block geometry, taken from the legacy `read_a0_rom`: `block_size = 96`; `num_blocks = 0x80000 / 96 = 5461` with a remainder of `32`, so `num_blocks` becomes **5462** and the final block is **32 bytes**. The request is `a0 00 <addr24> <len-1>` — six payload bytes, one fewer than the ECU sibling's seven, because this family omits the second `0x00`.

- [ ] **Step 1: Write the failing tests for block geometry and the tail block**

Append to the executor test:

```cpp
TEST(SubaruTcuHitachiM32rKlineExecutor, ReadsTheRomIn96ByteBlocksWithA32ByteTail)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    std::uint32_t blocks = 0;
    std::uint32_t last_length = 0;
    {
        const auto section = transport.section("read chunks");
        for (std::uint32_t address = 0; address < kRomSize; address += kBlockSize)
        {
            last_length = std::min(kBlockSize, kRomSize - address);
            ++blocks;
            transport.exchange(frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16),
                                      static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address),
                                      static_cast<bytes::Byte>(last_length - 1)}),
                               blockResponse(last_length, 0x5a));
        }
    }
    EXPECT_EQ(blocks, 5462u);
    EXPECT_EQ(last_length, 32u);

    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    ManualCancellationToken cancellation;
    fastecu::testing::RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    EXPECT_TRUE(transport.scriptConsumed());
}

// The legacy retried a block up to five times while the response was 5 bytes
// or shorter; the sixth failure gave up silently and produced a short ROM.
TEST(SubaruTcuHitachiM32rKlineExecutor, RetriesABlockUpToFiveTimes)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    const bytes::Bytes request = frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f});
    // Four short responses, then a good one: the fifth attempt succeeds.
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        transport.exchange(request, bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xe0});
    }
    transport.exchange(request, blockResponse(kBlockSize, 0x5a));
    for (std::uint32_t address = kBlockSize; address < kRomSize; address += kBlockSize)
    {
        const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
        transport.exchange(frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16),
                                  static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address),
                                  static_cast<bytes::Byte>(length - 1)}),
                           blockResponse(length, 0x5a));
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    ManualCancellationToken cancellation;
    fastecu::testing::RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
}

// Deliberate divergence 2: the legacy appended nothing and returned
// STATUS_SUCCESS, yielding a silently short ROM.
TEST(SubaruTcuHitachiM32rKlineExecutor, FailsWhenABlockExhaustsItsFiveAttempts)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    const bytes::Bytes request = frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f});
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        transport.exchange(request, bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xe0});
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    ManualCancellationToken cancellation;
    fastecu::testing::RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrorOfKind(ErrorKind::BadResponse));
}

// Deliberate divergence 3: the legacy accepted any response longer than five
// bytes and appended length-1 of it, misaligning every later byte.
TEST(SubaruTcuHitachiM32rKlineExecutor, RejectsABlockResponseOfTheWrongLength)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    transport.exchange(frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f}), blockResponse(kBlockSize - 8, 0x5a));

    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    ManualCancellationToken cancellation;
    fastecu::testing::RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrorOfKind(ErrorKind::BadResponse));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test`
Expected: all four new tests FAIL with `read_rom lands in task 3`.

- [ ] **Step 3: Replace the stub with the real read loop**

In `subaru_tcu_hitachi_m32r_kline_executor.cpp`, delete the stub and write:

```cpp
Result<bytes::Bytes> read_rom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruTcuHitachiM32rKlinePlan& p)
{
    bytes::Bytes rom;
    rom.reserve(kRomSize);
    for (std::uint32_t address = 0; address < kRomSize; address += p.block_size)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during ROM read");
        }
        const std::uint32_t length = std::min(p.block_size, kRomSize - address);
        const bytes::Bytes request =
            composeBe(0xa0_b, 0x00_b, bytes::u24(address), static_cast<bytes::Byte>(length - 1));

        // read_a0_rom's retry loop: up to five attempts while the response is
        // too short to carry data. Each attempt keeps the legacy delay(100)
        // on either side of the read.
        std::optional<bytes::Bytes> block;
        for (int attempt = 0; attempt < kBlockAttempts; ++attempt)
        {
            if (auto slept = clock.sleep(kBlockDelay, cancellation); !slept.has_value())
            {
                return std::unexpected(slept.error());
            }
            auto response = exchange(transport, cancellation, request, p, kBlockTimeoutMs);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (auto slept = clock.sleep(kBlockDelay, cancellation); !slept.has_value())
            {
                return std::unexpected(slept.error());
            }
            if (response->size() > 5)
            {
                block = std::move(*response);
                break;
            }
        }
        if (!block.has_value())
        {
            return fail(ErrorKind::BadResponse,
                        std::format("no block-read response at 0x{:06x} after {} attempts", address, kBlockAttempts));
        }
        // 5 header bytes, `length` data bytes, 1 checksum byte.
        if (block->size() != length + 6)
        {
            return fail(ErrorKind::BadResponse,
                        std::format("block read at 0x{:06x} returned {} bytes, expected {}", address, block->size(),
                                    length + 6));
        }
        rom.insert(rom.end(), block->begin() + 5, block->end() - 1);
        events.progress(static_cast<int>(address + length), static_cast<int>(kRomSize));
    }
    return rom;
}
```

Add `#include <algorithm>` and `#include <optional>` to the file's includes, and remove the forward declaration added in Task 2 if the definition now precedes its use.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test`
Expected: PASS, all six tests including Task 2's connect test.

- [ ] **Step 5: Commit**

```bash
git add src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.cpp \
        src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor_test.cpp
git commit -m "feat(flash): read the TCU Hitachi M32R ROM in 96-byte blocks"
```

---

### Task 4: Cancellation and failure paths

**Files:**
- Modify: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor_test.cpp`
- Test: `//src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test`

**Interfaces:**
- Consumes: everything from Tasks 2 and 3.
- Produces: no new symbols. This task proves the contract the `FlashWorker` depends on.

Tasks 2 and 3 already wrote the cancellation checks; this task proves them, and fixes them if the proof fails.

- [ ] **Step 1: Write the failing tests**

Append to the executor test:

```cpp
TEST(SubaruTcuHitachiM32rKlineExecutor, StopsPromptlyWhenCancelledMidRead)
{
    ManualCancellationToken cancellation;
    TripOnReadTransport transport{cancellation};
    scriptConnect(transport);
    {
        const auto section = transport.section("read chunks");
        for (std::uint32_t address = 0; address < kRomSize; address += kBlockSize)
        {
            const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
            transport.exchange(frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16),
                                      static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address),
                                      static_cast<bytes::Byte>(length - 1)}),
                               blockResponse(length, 0x5a));
        }
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    fastecu::testing::RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErrorOfKind(ErrorKind::Cancelled));
    EXPECT_LT(transport.writesConsumed(), 100u);
}

TEST(SubaruTcuHitachiM32rKlineExecutor, RejectsAPlanBuiltForAnotherFamily)
{
    auto foreign = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_ecu_hitachi_m32r_kline",
                                                        "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(foreign, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    ManualCancellationToken cancellation;
    fastecu::testing::RecordingEventSink events;

    EXPECT_THAT(executor.transport_setup(*foreign), fastecu::testing::IsError());
    EXPECT_THAT(executor.execute(*foreign, transport, clock, cancellation, events), fastecu::testing::IsError());
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

TEST(SubaruTcuHitachiM32rKlineExecutor, FailsWhenTheSeedResponseIsTooShort)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    const auto section = transport.section("connect");
    transport.exchange(frame({0xbf}), idResponse());
    transport.exchange(frame({0x81}), {0x80, 0xf0, 0x18, 0x01, 0xc1, 0});
    transport.exchange(frame({0x83, 0x00}), {0x80, 0xf0, 0x18, 0x01, 0xc3, 0});
    // 0x67 0x01 present but only two seed bytes follow.
    transport.exchange(frame({0x27, 0x01}), bytes::Bytes{0x80, 0xf0, 0x18, 0x04, 0x67, 0x01, 0xde, 0xad, 0});

    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    ManualCancellationToken cancellation;
    fastecu::testing::RecordingEventSink events;

    EXPECT_THAT(executor.execute(readPlan(), transport, clock, cancellation, events),
                fastecu::testing::IsErrorOfKind(ErrorKind::BadResponse));
}

TEST(SubaruTcuHitachiM32rKlineExecutor, FailsWhenTheTcuNeverAnswers)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    transport.expectWrite(frame({0xbf}));
    transport.queue_no_frame();

    SubaruTcuHitachiM32rKlineExecutor executor;
    fastecu::testing::FakeClock clock;
    ManualCancellationToken cancellation;
    fastecu::testing::RecordingEventSink events;

    EXPECT_THAT(executor.execute(readPlan(), transport, clock, cancellation, events),
                fastecu::testing::IsErrorOfKind(ErrorKind::Timeout));
}
```

Add the `TripOnReadTransport` helper at the top of the anonymous namespace, copied from `subaru_hitachi_m32r_kline_executor_test.cpp:19-38` — it cancels on the third `read()`.

- [ ] **Step 2: Run the tests**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test`
Expected: PASS. If `StopsPromptlyWhenCancelledMidRead` fails, a cancellation checkpoint is missing from Task 3's loop — add it there rather than weakening the assertion.

- [ ] **Step 3: Commit**

```bash
git add src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor_test.cpp
git commit -m "test(flash): cover TCU Hitachi M32R K-Line cancellation and failures"
```

---

### Task 5: Desktop routing

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp` (routing table ~line 863-940, factory switch ~line 969)
- Modify: `src/platform/desktop/common/flash/flash_workflow_test.cpp`
- Modify: `src/platform/desktop/common/flash/BUILD.bazel`
- Test: `//src/platform/desktop/common/flash:flash_workflow_test`

**Interfaces:**
- Consumes: Task 1's `build_subaru_tcu_hitachi_m32r_kline_plan`, Task 2's `SubaruTcuHitachiM32rKlineExecutor`; existing `bind_flash_attempt`, `DesktopKlineFlashTransport`, `QtClock`, `FlashWorkflow`.
- Produces: `Route::Kind::SubaruTcuHitachiM32rKline`, routing for the exact protocol `sub_tcu_hitachi_m32r_kline`.

Route as `RouteMatch::Exact`. The existing entry for `sub_tcu_hitachi_m32r_can` is a different protocol, but exact matching keeps the two independent of table order.

- [ ] **Step 1: Write the failing routing test**

In `flash_workflow_test.cpp`, add `"sub_tcu_hitachi_m32r_kline"` to the list of protocols asserted routable (around line 238), and add:

```cpp
void routesTcuHitachiM32rKlineReadOnly()
{
    QVERIFY(FlashWorkflowFactory::tryCreate(request("sub_tcu_hitachi_m32r_kline")) != nullptr);
    // Write is rejected by the plan, so the workflow's first step is a failure
    // rather than an attempt.
    auto write_request = request("sub_tcu_hitachi_m32r_kline");
    write_request.operation = FlashOperation::Write;
    write_request.image = bytes::Bytes(0x80000, 0x00);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(write_request));
    QVERIFY(workflow != nullptr);
    QVERIFY(std::holds_alternative<FlashFailureStep>(workflow->next()));
}
```

Register it in the test class's slot list following the file's existing convention.

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/flash:flash_workflow_test`
Expected: FAIL — `tryCreate` returns `nullptr` because no route matches.

- [ ] **Step 3: Add the workflow class**

In `flash_workflow.cpp`, next to `SubaruM32rKlineWorkflow`:

```cpp
class SubaruTcuHitachiM32rKlineWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruTcuHitachiM32rKlineWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)),
          plan_(build_subaru_tcu_hitachi_m32r_kline_plan(request_.operation, request_.protocol, request_.mcu,
                                                         std::move(request_.image)))
    {
    }
    FlashWorkflowStep next() override
    {
        if (!plan_)
        {
            return FlashFailureStep{plan_.error()};
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
            attempted_ = true;
            return FlashWorkflowStep{
                std::in_place_type<FlashAttempt>,
                bind_flash_attempt(std::move(*plan_), std::make_unique<SubaruTcuHitachiM32rKlineExecutor>(),
                                   std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                std::make_unique<QtClock>()};
        }
        return outcome_.completedStep();
    }
    void submit(FlashPromptResponse response) override
    {
        begun_ = true;
        if (response != FlashPromptResponse::Accept)
        {
            outcome_.cancel();
        }
    }
    void submit(FlashAttemptResult result) override
    {
        outcome_.record(std::move(result));
    }

  private:
    FlashWorkflowRequest request_;
    Result<FlashPlan> plan_;
    bool begun_ = false;
    bool attempted_ = false;
    FlashAttemptOutcome outcome_;
};
```

Add `SubaruTcuHitachiM32rKline` to `Route::Kind`, the row `{"sub_tcu_hitachi_m32r_kline", SubaruTcuHitachiM32rKline, RouteMatch::Exact},` to `kRoutes`, the matching `case` to the factory switch, the executor and plan headers to the file's includes, and the two new `cc_library` labels to the package's `deps`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `bazel test --config=release //src/platform/desktop/common/flash:flash_workflow_test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/platform/desktop/common/flash/flash_workflow.cpp \
        src/platform/desktop/common/flash/flash_workflow_test.cpp \
        src/platform/desktop/common/flash/BUILD.bazel
git commit -m "feat(flash): route sub_tcu_hitachi_m32r_kline to the portable workflow"
```

---

### Task 6: Delete the legacy family and close the row

**Files:**
- Delete: `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.{h,cpp}`
- Delete: `src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_kline.{h,cpp}`
- Modify: `src/ui/desktop/mainwindow.cpp:1343-1347`, `src/ui/desktop/flash/tcu/BUILD.bazel`, `scripts/check-legacy-flash-drain.py`, `docs/flash-qualification-matrix.md`
- Test: `//...`

**Interfaces:**
- Consumes: Task 5's routing — this is what makes the legacy path unreachable.
- Produces: one fewer `REMAINING` entry.

- [ ] **Step 1: Delete the legacy sources and the dialog**

```bash
git rm src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.h \
       src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.cpp \
       src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_kline.h \
       src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_kline.cpp
```

The legacy package's `BUILD.bazel` needs no edit — its `srcs` and `MOC_HDRS` are globs. `src/ui/desktop/flash/tcu/BUILD.bazel` lists files explicitly: remove the two names there, and remove the `#include` and the `FlashTcuSubaruHitachiM32rKline` branch at `mainwindow.cpp:1343-1347`.

- [ ] **Step 2: Shrink the ratchet**

Remove exactly this line from `REMAINING` in `scripts/check-legacy-flash-drain.py`:

```python
    "tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.cpp",
```

- [ ] **Step 3: Run the guard**

Run: `bazel test --config=release //:legacy_flash_drain`
Expected: PASS. A "drain shrank" message means the entry was left in `REMAINING`; a "drain grew" message means a file was missed.

- [ ] **Step 4: Update the qualification matrix**

In `docs/flash-qualification-matrix.md`, set the `FlashTcuSubaruHitachiM32rKline` row to `portable = yes`, `operations = read`, `automated_evidence` naming `subaru_tcu_hitachi_m32r_kline_plan_test`, `subaru_tcu_hitachi_m32r_kline_executor_test` and `flash_workflow_test`, and `hardware_status = experimental`. In `notes`, record the four deliberate divergences — especially that the legacy write path reported success without writing — and the unverified `M32R_512KB` MCU binding.

- [ ] **Step 5: Run the whole suite**

Run: `bazel test --config=release //...`
Expected: PASS, every target.

- [ ] **Step 6: Run the linters and the changed-file clang-tidy gate**

Run: `prek run --all-files` then `bazel run //:clang_tidy_report_changed`
Expected: both clean.

- [ ] **Step 7: Commit and open the PR**

```bash
git add -A
git commit -m "refactor(flash): delete the legacy TCU Hitachi M32R K-Line operation"
git push -u origin flash/wave6a1-tcu-hitachi-m32r-kline
gh pr create --title "feat(flash): modularize the Subaru TCU Hitachi M32R K-Line family" --body "…"
```

The PR body must list the four deliberate divergences from the top of this plan, and the `VERIFY` line for the `M32R_512KB` MCU binding.

---

## Self-Review

**Spec coverage.** The spec's 6a-1 row is this whole plan. Its per-PR anatomy — enum value, plan POD, variant alternative, plan and executor pairs, dialog rewrite, legacy deletion, ratchet entry, matrix row — maps to Tasks 1, 2, 3, 5 and 6. Its three test layers map to Tasks 1 (plan), 2–4 (executor) and 5 (desktop). The spec's stop condition for 6a is honored: this family needs no port addition, and `supports_write` is a shared-helper field, not a port change. The spec's behavior-correction rule is where the four divergences came from; they belong in the spec's appendix when this PR lands.

**Deviation from the spec worth noting at review time.** The spec says 6a requires "no port changes, no `bazel/` changes, no ADR". This plan adds a field to `SingleWindowPlanSpec`, which is neither a port nor a `bazel/` change — but it is a shared-helper change the spec did not anticipate, because the spec did not yet know this family's write path was unimplemented. It is additive with a safe default and covered by running the full `//src/backend/flash/...` suite in Task 1 Step 9.

**Placeholders.** None. Every code step carries the code. The one unresolved input — the MCU name — is named explicitly, with its evidence, its chosen value, and its blast radius, rather than left as a TODO.

**Type consistency.** `SubaruTcuHitachiM32rKlinePlan` carries `tester_id`, `target_id`, `baud`, `block_size` in Tasks 1, 2 and 3 alike. `build_subaru_tcu_hitachi_m32r_kline_plan` and `validate_subaru_tcu_hitachi_m32r_kline_plan` keep their Task 1 signatures in Tasks 2 and 5. `read_rom` has the same five parameters in its Task 2 stub, its Task 2 forward declaration and its Task 3 definition. `FlashFamily::SubaruTcuHitachiM32rKline` and `Route::Kind::SubaruTcuHitachiM32rKline` are distinct types with deliberately matching names, following the established convention.
