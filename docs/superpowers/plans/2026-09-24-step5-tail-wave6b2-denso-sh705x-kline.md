# Wave 6b-2 — Subaru ECU Denso SH705x K-Line — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land `IKlineFlashTransport::reset_connection()` and a shared SH705x K-Line crypto-table header (PR 6b-2a), then migrate `FlashEcuSubaruDensoSH705xKlineOperation` to a portable plan and `IKlineFlashExecutor` routed through `FlashWorkflow`, taking `//:legacy_flash_drain` from five entries to four (PR 6b-2b).

**Architecture:** PR 6b-2a adds the K-Line mirror of `ICanFlashTransport::reset_connection()` to the port, the desktop adapter and the scripted fake, and moves three byte-identical crypto tables into a header-only `denso_sh705x_kline_common` target that the already-ported EEPROM K-Line executor adopts. PR 6b-2b adds a family plan (six exact protocol/MCU pairs, Cobb gated to TestWrite) and an executor that resets in `before_transport_configure()`, then probes for a live kernel, runs the SSM bootloader handshake, uploads a `0x5AA5`-balanced kernel, and reads or CRC-compares and reflashes changed blocks. A new desktop workflow replaces the legacy dialog and both `MainWindow` branches.

**Tech Stack:** C++23, Bazel, GoogleTest (`fastecu_portable_gtest`), QtTest for desktop adapter/workflow suites, gmock `FakeBackend` for `SerialPortActions`.

**Spec:** [Wave 6b-2 — Denso SH705x K-Line — Design](../specs/2026-09-24-step5-tail-wave6b2-denso-sh705x-kline-design.md). Parent: [Wave 6 singletons](../specs/2026-09-19-step5-tail-wave6-singletons-design.md).

## Global Constraints

- Behavior-correction policy is the 6a-3/6a-4 exception: preserve wire bytes, order and timing; correct only bounds, read-integrity and reply-gating defects, each named in the matrix notes. The parent spec's blanket preservation rule and "Preserved Legacy Defects" appendix do not govern this family.
- Shared substrate is identical data only: the seed-key table, the encrypt table and their three `SsmProtocol` wrappers. No protocol function is shared with the EEPROM executor.
- Two PRs: 6b-2a (foundation, no behavior change, drain unchanged) then 6b-2b (family, drain −1).
- The executor never calls `configure()`, `open()` or `close()` (ADR 0015). The reset happens in `before_transport_configure()`.
- All wire writes use `write()` (echo-checked), never `write_raw()`/`read_raw()`.
- Backend operations return `fastecu::Result<T>`, checked with `.has_value()`, never implicit `operator bool`. Exceptions never cross a port. Do not add an `ErrorKind`.
- Pure protocol code uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView`. The backend never sees `EcuCalDefStructure`, Qt, threads or the filesystem.
- Every new backend `cc_library` is registered by name in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`).
- Add no ratchet entry. In 6b-2b remove exactly `ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp` from `REMAINING` in `scripts/check-legacy-flash-drain.py`.
- Every executor exchange carries a comment citing the legacy function and line in `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp`. That file exists until Task 9 deletes it; read the cited lines while it does. Legacy log strings are copied verbatim from the cited `emit LOG_I` / `LOG_E` lines.
- Any test claiming to pin a correction or the TestWrite opcode choice is mutation-checked: change the production branch/value, run the named test, observe it fail, restore a byte-identical tree (`git diff --exit-code` on that file).
- Tests are package-owned and co-located. Mocks and fakes stay package-owned.
- Markdown cross-references are links with human-readable text, not backticked paths.
- Work lands through pull requests. Do not push or open a PR without explicit user approval.

## Legacy constants (read from the header and body; transcribed here once)

| Name | Value | Legacy source |
|---|---|---|
| `serial_read_timeout` | 2000 ms | header :48 |
| `serial_read_short_timeout` | 200 ms | header :50 |
| `serial_read_medium_timeout` | 500 ms | header :51 |
| `serial_read_long_timeout` | 800 ms | header :52 |
| `serial_read_extra_long_timeout` | 3000 ms | header :53 |
| tester / target | 0xF0 / 0x10 | `execute()` :73-76 |
| initial baud | 4800 | `execute()` :72 |
| probe baud / settle | 62500 / 100 ms | `connect_bootloader()` :127-128 |
| SSM baud / settle | 4800 / 100 ms | `connect_bootloader()` :161-162 |
| upload baud | 15625 | `upload_kernel()` :354 |
| post-`31` settle, then baud | 100 ms, 62500 | `upload_kernel()` :461-463 |
| kernel-ID settle / timeout | 200 ms / 800 ms | `request_kernel_id()` :1711-1713 |
| CRC inter-block pacing | 5 ms | `get_changed_blocks()` `delay(5)` |
| read page | 0x400 | `read_mem()` |
| write chunk / commit block | 0x200 / 0x1000 | `flash_block()` (`blocksize`, `flashblocksize`) |
| kernel upload chunk | 0x80 | `send_sid_36_transferdata()` |
| kernel opcodes | ID 0x01, CRC 0x02, READ_AREA 0x03, PROG_VOLT 0x04, MAX_MSG 0x05, MAX_BLK 0x06, FLASH_ENABLE 0x20, FLASH_DISABLE 0x21, WRITE_BUF 0x22, VALIDATE 0x23, COMMIT 0x24, BLANK_PAGE 0x25 | `src/backend/definitions/kernelcomms.h` |

## File map

**PR 6b-2a**
- Modify `src/backend/flash/flash_executor.h` — `reset_connection()` on `IKlineFlashTransport`.
- Modify `src/backend/flash/testing/scripted_kline_flash_transport.h` — record lifecycle calls incl. reset.
- Modify `src/backend/flash/testing/scripted_flash_transports_test.cpp`.
- Modify `src/platform/desktop/common/transport/desktop_kline_flash_transport.{h,cpp}` and `_test.cpp`.
- Create `src/backend/flash/ecu/denso_sh705x_kline_common.h` + `_test.cpp`; modify `src/backend/flash/ecu/BUILD.bazel`.
- Modify `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp` and `src/backend/flash/eeprom/BUILD.bazel`.
- Modify `bazel/portable_targets.bzl`.

**PR 6b-2b**
- Create `src/backend/flash/ecu/subaru_denso_sh705x_kline_{types.h,plan.h,plan.cpp,plan_test.cpp,executor.h,executor.cpp,executor_test.cpp}`.
- Modify `src/backend/flash/{flash_types.h,flash_plan.cpp,flash_validation_test.cpp,BUILD.bazel}`, `src/backend/flash/testing/flash_printers.h`, `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`.
- Modify `src/platform/desktop/common/flash/{flash_workflow.cpp,flash_workflow_test.cpp,BUILD.bazel}`.
- Modify `src/ui/desktop/{mainwindow.cpp,mainwindow.h}`, `src/ui/desktop/flash/ecu/BUILD.bazel`.
- Delete `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_kline.{h,cpp}` and `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.{h,cpp}`.
- Modify `scripts/check-legacy-flash-drain.py`, `docs/flash-qualification-matrix.md`, the parent spec; create `docs/denso-sh705x-kline-bench-checklist.md`.

---

## PR 6b-2a — foundation

### Task 0: Branch

- [ ] **Step 1: Rename the spec branch to the foundation branch**

The spec and this plan are committed on `docs/wave6b2-denso-sh705x-kline-design`; they travel with 6b-2a.

```bash
git branch -m docs/wave6b2-denso-sh705x-kline-design feat/wave6b2a-kline-reset-foundation
git status --short   # expect clean
```

### Task 1: `IKlineFlashTransport::reset_connection()`

**Files:**
- Modify: `src/backend/flash/flash_executor.h` (class `IKlineFlashTransport`, after `close()`)
- Modify: `src/backend/flash/testing/scripted_kline_flash_transport.h`
- Test: `src/backend/flash/testing/scripted_flash_transports_test.cpp`
- Modify: `src/platform/desktop/common/transport/desktop_kline_flash_transport.h`, `.cpp`
- Test: `src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp`

**Interfaces:**
- Produces: `virtual Status IKlineFlashTransport::reset_connection() = 0;`
- Produces on `ScriptedKlineFlashTransport`: `std::vector<std::string> lifecycle_calls_` (entries `"reset_connection"`, `"configure"`, `"open"`, `"close"`), `int reset_call_count_`, `Status reset_result_`.

- [ ] **Step 1: Write the failing scripted-transport test**

Append to `scripted_flash_transports_test.cpp`, next to the other `ScriptedKlineFlashTransport` tests:

```cpp
TEST(ScriptedKlineFlashTransport, RecordsResetInLifecycleOrderAndClosesThePort)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};

    ASSERT_TRUE(transport.reset_connection().has_value());
    EXPECT_FALSE(transport.isOpen());
    ASSERT_TRUE(transport.configure(KlineConfig{.baud = 4800, .iso14230 = false, .tester_id = 0xF0, .target_id = 0x10})
                    .has_value());
    ASSERT_TRUE(transport.open().has_value());
    ASSERT_TRUE(transport.close().has_value());

    EXPECT_EQ(transport.reset_call_count_, 1);
    EXPECT_THAT(transport.lifecycle_calls_, ::testing::ElementsAre("reset_connection", "configure", "open", "close"));
}

TEST(ScriptedKlineFlashTransport, ResetReturnsItsScriptedFailure)
{
    ScriptedKlineFlashTransport transport;
    transport.reset_result_ = fail(ErrorKind::Disconnected, "scripted reset failure");

    const Status reset = transport.reset_connection();

    ASSERT_FALSE(reset.has_value());
    EXPECT_EQ(reset.error().kind, ErrorKind::Disconnected);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/backend/flash/testing:scripted_flash_transports_test`
Expected: compile failure — `reset_connection`, `lifecycle_calls_`, `reset_call_count_`, `reset_result_` are not members.

- [ ] **Step 3: Add the port method**

In `flash_executor.h`, inside `class IKlineFlashTransport`, directly after `virtual Status close() = 0;`:

```cpp
    // Every K-Line transport must expose a real reset so protocol-owned
    // sequences cannot silently degrade into configure/open only. Mirrors
    // ICanFlashTransport::reset_connection(). Called only from an executor's
    // before_transport_configure(), never mid-session: the caller still owns
    // configure/open/close (ADR 0015).
    virtual Status reset_connection() = 0;
```

- [ ] **Step 4: Implement it in the scripted transport**

In `scripted_kline_flash_transport.h`, add `reset_connection()` above `configure()` and record lifecycle calls in `configure()`, `open()` and `close()`:

```cpp
    Status reset_connection() override
    {
        lifecycle_calls_.push_back("reset_connection");
        ++reset_call_count_;
        open_ = false;
        return reset_result_;
    }

    Status configure(const KlineConfig& config) override
    {
        lifecycle_calls_.push_back("configure");
        last_config_ = config;
        return configure_result_;
    }
    Status open() override
    {
        lifecycle_calls_.push_back("open");
        open_ = true;
        return open_result_;
    }
    Status close() override
    {
        lifecycle_calls_.push_back("close");
        ++close_call_count_;
        open_ = false;
        return close_result_;
    }
```

Add the public members next to `close_call_count_`:

```cpp
    // Lifecycle calls in order, as ScriptedCanFlashTransport records them, so
    // a test can pin a reset before configure().
    std::vector<std::string> lifecycle_calls_;
    int reset_call_count_ = 0;
    Status reset_result_;
```

- [ ] **Step 5: Write the failing desktop-adapter tests**

Add two slots to `TestDesktopKlineFlashTransport` in `desktop_kline_flash_transport_test.cpp`:

```cpp
    // reset_connection() is the SH705x K-Line startup seam. It calls the real
    // SerialPortActions facade over FakeBackend, as the CAN adapter test does.
    void resetConnectionReachesTheAdapter()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), reset_connection()).WillOnce(::testing::Return());

        DesktopKlineFlashTransport transport(serial.release());

        QVERIFY(transport.reset_connection().has_value());
    }

    void resetConnectionAfterCloseIsDisconnectedAndTouchesNoBackend()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), reset_connection()).Times(0);

        DesktopKlineFlashTransport transport(serial.get()); // non-owning: keep `serial` alive
        QVERIFY(transport.close().has_value());
        const auto result = transport.reset_connection();

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }
```

- [ ] **Step 6: Run to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/transport:desktop_kline_flash_transport_test`
Expected: compile failure — `DesktopKlineFlashTransport` is abstract (`reset_connection` not overridden).

- [ ] **Step 7: Implement the desktop adapter**

In `desktop_kline_flash_transport.h`, add `Status reset_connection() override;` directly after `Status close() override;`. In the `.cpp`, add after `close()`:

```cpp
Status DesktopKlineFlashTransport::reset_connection()
{
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "reset_connection() called after close()");
    }
    try
    {
        // No real sentinel: SerialPortActions::reset_connection()
        // (serial_port_actions.cpp:541-544) is `runOnBackend(...); return
        // true;` -- it cannot report failure through its return value, so
        // this branch is unreachable today and only the surrounding catch
        // blocks below can produce an Internal error here.
        if (!serial_->reset_connection())
        {
            return fail(ErrorKind::Internal, "reset_connection failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "reset_connection exception");
    }
}
```

- [ ] **Step 8: Run both suites and the full K-Line surface**

Run:
```bash
bazel test --config=release //src/backend/flash/testing:scripted_flash_transports_test \
  //src/platform/desktop/common/transport:desktop_kline_flash_transport_test
bazel build --config=release //...
```
Expected: both suites PASS; the full build succeeds (the port has exactly two implementers, both updated).

- [ ] **Step 9: Commit**

```bash
git add src/backend/flash/flash_executor.h src/backend/flash/testing/scripted_kline_flash_transport.h \
  src/backend/flash/testing/scripted_flash_transports_test.cpp \
  src/platform/desktop/common/transport/desktop_kline_flash_transport.h \
  src/platform/desktop/common/transport/desktop_kline_flash_transport.cpp \
  src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp
git commit -m "feat(flash): add reset_connection to the K-Line flash transport port"
```

### Task 2: Shared SH705x K-Line crypto tables

**Files:**
- Create: `src/backend/flash/ecu/denso_sh705x_kline_common.h`
- Create: `src/backend/flash/ecu/denso_sh705x_kline_common_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`
- Modify: `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp:114-138`, `src/backend/flash/eeprom/BUILD.bazel` (target `denso_sh705x_eeprom_kline`)

**Interfaces:**
- Produces: `kDensoSh705xKlineSeedKeyTable` (`std::array<std::uint16_t, 16>`), `kDensoSh705xKlineEncryptTable` (`std::array<std::uint16_t, 4>`), `bytes::Bytes denso_sh705x_kline_stock_seed_key(bytes::ByteView seed)`, `bytes::Bytes denso_sh705x_kline_ecutek_seed_key(bytes::ByteView seed)`, `bytes::Bytes denso_sh705x_kline_encrypt_payload(bytes::ByteView buf, std::uint32_t len)`, all in `namespace fastecu::flash`; Bazel target `//src/backend/flash/ecu:denso_sh705x_kline_common`.

- [ ] **Step 1: Write the failing test**

`denso_sh705x_kline_common_test.cpp` — literals transcribed independently from the legacy `generate_seed_key()` / `generate_ecutek_seed_key()` / `encrypt_payload()` of the flash family, so the test does not read the header back:

```cpp
#include "src/backend/flash/ecu/denso_sh705x_kline_common.h"

#include <array>
#include <cstdint>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{
namespace
{

// flash_ecu_subaru_denso_sh705x_kline_operation.cpp generate_seed_key() /
// generate_ecutek_seed_key(): the same 16-entry key table.
constexpr auto kLegacyKeyTable =
    std::to_array<std::uint16_t>({0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608, 0xAFB8, 0x503D,
                                  0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0});
// encrypt_payload(): the 4-entry key table.
constexpr auto kLegacyEncryptTable = std::to_array<std::uint16_t>({0x7856, 0xCE22, 0xF513, 0x6E86});

const bytes::Bytes kSeed{0x12, 0x34, 0x56, 0x78};

TEST(DensoSh705xKlineCommon, TablesMatchTheLegacyLiterals)
{
    EXPECT_EQ(kDensoSh705xKlineSeedKeyTable, kLegacyKeyTable);
    EXPECT_EQ(kDensoSh705xKlineEncryptTable, kLegacyEncryptTable);
}

TEST(DensoSh705xKlineCommon, StockSeedKeyUsesTheStockTransformation)
{
    EXPECT_EQ(denso_sh705x_kline_stock_seed_key(kSeed),
              SsmProtocol::calculateSeedKey(kSeed, kLegacyKeyTable, SsmProtocol::kIndexTransformationStock));
}

TEST(DensoSh705xKlineCommon, EcutekSeedKeyUsesTheEcutekTransformation)
{
    const bytes::Bytes stock = denso_sh705x_kline_stock_seed_key(kSeed);
    const bytes::Bytes ecutek = denso_sh705x_kline_ecutek_seed_key(kSeed);

    EXPECT_EQ(ecutek, SsmProtocol::calculateSeedKey(kSeed, kLegacyKeyTable, SsmProtocol::kIndexTransformationEcutek));
    EXPECT_NE(ecutek, stock);
}

TEST(DensoSh705xKlineCommon, EncryptPayloadUsesTheKlineTableAndStockTransformation)
{
    const bytes::Bytes payload{0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x8D, 0xC8};
    EXPECT_EQ(denso_sh705x_kline_encrypt_payload(payload, 8),
              SsmProtocol::calculatePayload(payload, 8, kLegacyEncryptTable, SsmProtocol::kIndexTransformationStock));
}

} // namespace
} // namespace fastecu::flash
```

Add to `src/backend/flash/ecu/BUILD.bazel`:

```python
cc_library(
    name = "denso_sh705x_kline_common",
    hdrs = ["denso_sh705x_kline_common.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/ssm",
    ],
)

fastecu_portable_gtest(
    name = "denso_sh705x_kline_common_test",
    srcs = ["denso_sh705x_kline_common_test.cpp"],
    deps = [
        ":denso_sh705x_kline_common",
        "//src/algorithms/protocol/ssm",
    ],
)
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_sh705x_kline_common_test`
Expected: FAIL — `denso_sh705x_kline_common.h` not found.

- [ ] **Step 3: Write the header**

```cpp
#pragma once
#include <array>
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

// Crypto tables shared by the two Denso SH705x K-Line executors:
// DensoSh705xEepromKlineExecutor (step 5c) and SubaruDensoSh705xKlineExecutor
// (wave 6b-2). Both legacy classes spelled out these exact tables; nothing
// else about the two protocols is shared -- their kernel upload, reply
// checks and timeouts differ on the wire. See the wave 6b-2 design.
//
// The executor suites do NOT read these back: each carries its own
// transcribed literals, so a wrong change here fails those suites.
namespace fastecu::flash
{

inline constexpr std::array<std::uint16_t, 16> kDensoSh705xKlineSeedKeyTable{
    0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608,
    0xAFB8, 0x503D, 0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0};

inline constexpr std::array<std::uint16_t, 4> kDensoSh705xKlineEncryptTable{0x7856, 0xCE22, 0xF513, 0x6E86};

inline bytes::Bytes denso_sh705x_kline_stock_seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kDensoSh705xKlineSeedKeyTable, SsmProtocol::kIndexTransformationStock);
}

inline bytes::Bytes denso_sh705x_kline_ecutek_seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kDensoSh705xKlineSeedKeyTable, SsmProtocol::kIndexTransformationEcutek);
}

inline bytes::Bytes denso_sh705x_kline_encrypt_payload(bytes::ByteView buf, std::uint32_t len)
{
    return SsmProtocol::calculatePayload(buf, len, kDensoSh705xKlineEncryptTable,
                                         SsmProtocol::kIndexTransformationStock);
}

} // namespace fastecu::flash
```

Register `"denso_sh705x_kline_common"` in the `"src/backend/flash/ecu"` list of `bazel/portable_targets.bzl`, alphabetically after `"denso_iso15765_can_common"`.

- [ ] **Step 4: Run to verify it passes**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_sh705x_kline_common_test`
Expected: PASS (4 tests).

- [ ] **Step 5: Adopt it in the EEPROM K-Line executor**

In `denso_sh705x_eeprom_kline_executor.cpp`, delete the local `generate_stock_seed_key`, `generate_ecutek_seed_key` and `encrypt_kernel_payload` definitions (lines 114-138), add `#include "src/backend/flash/ecu/denso_sh705x_kline_common.h"`, and replace the three call sites:

```cpp
    const bytes::Bytes seed_key = kline_plan.security == DensoSecurityVariant::EcuTek
                                      ? denso_sh705x_kline_ecutek_seed_key(seed)
                                      : denso_sh705x_kline_stock_seed_key(seed);
```
```cpp
    const bytes::Bytes encrypted_kernel = denso_sh705x_kline_encrypt_payload(padded_kernel, pl_len);
```
```cpp
    const bytes::Bytes encrypted_bypass = denso_sh705x_kline_encrypt_payload(cks_bypass, 4);
```

Add `"//src/backend/flash/ecu:denso_sh705x_kline_common"` to the `deps` of `denso_sh705x_eeprom_kline` in `src/backend/flash/eeprom/BUILD.bazel`. Do not edit `denso_sh705x_eeprom_kline_executor_test.cpp` — its independent literals are the guard.

- [ ] **Step 6: Run the EEPROM suite and the closure guard**

Run:
```bash
bazel test --config=release //src/backend/flash/eeprom:all //src/backend/flash/ecu:denso_sh705x_kline_common_test
bazel build --config=release //:portable_closure
git diff --stat -- src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor_test.cpp   # expect no output
```
Expected: all PASS; closure builds; the EEPROM test file is untouched.

- [ ] **Step 7: Mutation check**

Change `kDensoSh705xKlineEncryptTable`'s first entry to `0x7857`. Run `bazel test --config=release //src/backend/flash/eeprom:denso_sh705x_eeprom_kline_executor_test` — expect a FAIL in its kernel-upload wire assertion. Restore and confirm `git diff --exit-code -- src/backend/flash/ecu/denso_sh705x_kline_common.h`.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/ecu/denso_sh705x_kline_common.h src/backend/flash/ecu/denso_sh705x_kline_common_test.cpp \
  src/backend/flash/ecu/BUILD.bazel bazel/portable_targets.bzl \
  src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp src/backend/flash/eeprom/BUILD.bazel
git commit -m "refactor(flash): share the SH705x K-Line crypto tables"
```

### Task 3: 6b-2a gates and PR

- [ ] **Step 1: Run the gates**

```bash
bazel test --config=release //...
bazel build --config=release //:fastecu //:portable_closure
prek run --all-files
bazel run //:clang_tidy_report_changed
python3 scripts/check-legacy-flash-drain.py   # expect "OK: 5 families remaining, none added."
```
Expected: all green. If `//tests:serial_backend_tests` crashes on Windows CI only, rerun it — that is a known pre-existing flake.

- [ ] **Step 2: Ask the user for approval to push, then open the PR**

After approval:
```bash
git push -u origin feat/wave6b2a-kline-reset-foundation
gh pr create --title "feat(flash): K-Line reset_connection and shared SH705x K-Line crypto (wave 6b-2a)" --body "$(cat <<'EOF'
### What
- add `IKlineFlashTransport::reset_connection()`, bound to `SerialPortActions::reset_connection()` and recorded by the scripted transport
- share the byte-identical SH705x K-Line seed-key and encrypt tables in `denso_sh705x_kline_common.h`; the EEPROM K-Line executor adopts it
- wave 6b-2 design and implementation plan

### Why
- foundation for migrating `FlashEcuSubaruDensoSH705xKline` (wave 6b-2b), whose legacy startup resets the adapter before configuring it
- no behavior change; `//:legacy_flash_drain` is unchanged

### Verification
- bazel test --config=release //...
- bazel build --config=release //:fastecu //:portable_closure
- prek run --all-files; bazel run //:clang_tidy_report_changed
- mutation check: altering the shared encrypt table fails the unchanged EEPROM executor suite

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## PR 6b-2b — the family

Branch after 6b-2a merges: `git switch master && git pull && git switch -c feat/wave6b2b-denso-sh705x-kline`.

### Task 4: Plan, family registration, validation

**Files:**
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_kline_types.h`
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h`, `.cpp`, `_test.cpp`
- Modify: `src/backend/flash/flash_types.h`, `src/backend/flash/flash_plan.cpp`, `src/backend/flash/flash_validation_test.cpp`, `src/backend/flash/testing/flash_printers.h`, `src/backend/flash/BUILD.bazel`, `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Produces:
  ```cpp
  enum class SubaruDensoSh705xKlineSeedKey { Stock, EcuTek };
  struct SubaruDensoSh705xKlinePlan {
      int initial_baud;          // 4800
      std::uint8_t tester_id;    // 0xF0
      std::uint8_t target_id;    // 0x10
      SubaruDensoSh705xKlineSeedKey seed_key;
  };
  FlashFamily::SubaruDensoSh705xKline   // TransportKind::Kline, kernel required (default)
  Result<FlashPlan> build_subaru_denso_sh705x_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                         std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                         KernelImage kernel);
  Status validate_subaru_denso_sh705x_kline_plan(const FlashPlan& plan);
  ```
- Plan invariants later tasks rely on: `transfer_region() == {0, romsize}`; `image()` present and exactly `romsize` for Write/TestWrite, absent for Read; `kernel()` present, non-empty, load address `0xFFFF6004` (SH7055) or `0xFFFF3000` (SH7058); every flash block length a multiple of 0x1000 and block 0 starting at 0; no erase regions, no confirmations.

- [ ] **Step 1: Write the failing plan tests**

`subaru_denso_sh705x_kline_plan_test.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"

#include <array>
#include <string_view>
#include <tuple>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

KernelImage kernel_for(std::string_view mcu)
{
    return KernelImage{.id = "kernel",
                       .load_address = mcu == "SH7055" ? 0xFFFF6004U : 0xFFFF3000U,
                       .bytes = {0xAA, 0xBB, 0xCC, 0xDD}};
}

std::uint32_t romsize(std::string_view mcu)
{
    return find_flash_device(mcu)->romsize;
}

bytes::Bytes image_for(std::string_view mcu)
{
    return bytes::Bytes(romsize(mcu), 0xFF);
}

struct Pair
{
    std::string_view protocol;
    std::string_view mcu;
    SubaruDensoSh705xKlineSeedKey seed_key;
    bool cobb;
};

constexpr auto kPairs = std::to_array<Pair>({
    {"sub_ecu_denso_sh7055_04", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock, false},
    {"sub_ecu_denso_sh7055_04_ecutek", "SH7055", SubaruDensoSh705xKlineSeedKey::EcuTek, false},
    {"sub_ecu_denso_sh7055_04_cobb", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock, true},
    {"sub_ecu_denso_sh7058", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock, false},
    {"sub_ecu_denso_sh7058_ecutek", "SH7058", SubaruDensoSh705xKlineSeedKey::EcuTek, false},
    {"sub_ecu_denso_sh7058_cobb", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock, true},
});

TEST(SubaruDensoSh705xKlinePlan, MapsAllSixPairsWithLegacyWireParameters)
{
    for (const Pair& pair : kPairs)
    {
        SCOPED_TRACE(pair.protocol);
        const auto plan = build_subaru_denso_sh705x_kline_plan(FlashOperation::TestWrite, pair.protocol, pair.mcu,
                                                               image_for(pair.mcu), kernel_for(pair.mcu));
        ASSERT_THAT(plan, IsOk());
        EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoSh705xKline);
        EXPECT_EQ(plan->transport(), TransportKind::Kline);
        EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, romsize(pair.mcu)}));
        EXPECT_TRUE(plan->erase_regions().empty());
        EXPECT_TRUE(plan->confirmations().empty());
        const auto& family = std::get<SubaruDensoSh705xKlinePlan>(plan->family_plan());
        // execute():67-76 -- 4800 baud, tester 0xF0, target 0x10.
        EXPECT_EQ(family.initial_baud, 4800);
        EXPECT_EQ(family.tester_id, 0xF0);
        EXPECT_EQ(family.target_id, 0x10);
        // connect_bootloader():269 -- flash method endsWith("_ecutek").
        EXPECT_EQ(family.seed_key, pair.seed_key);
    }
}

TEST(SubaruDensoSh705xKlinePlan, ReadCarriesNoImage)
{
    const auto plan = build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, "sub_ecu_denso_sh7058", "SH7058",
                                                           image_for("SH7058"), kernel_for("SH7058"));
    ASSERT_THAT(plan, IsOk());
    EXPECT_FALSE(plan->image().has_value());
}

TEST(SubaruDensoSh705xKlinePlan, CobbIsTestWriteOnly)
{
    for (const std::string_view protocol : {"sub_ecu_denso_sh7055_04_cobb", "sub_ecu_denso_sh7058_cobb"})
    {
        SCOPED_TRACE(protocol);
        const std::string_view mcu = protocol.find("sh7055") != std::string_view::npos ? "SH7055" : "SH7058";
        EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, protocol, mcu, std::nullopt,
                                                         kernel_for(mcu)),
                    IsErr(ErrorKind::Unsupported));
        EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Write, protocol, mcu, image_for(mcu),
                                                         kernel_for(mcu)),
                    IsErr(ErrorKind::Unsupported));
    }
}

TEST(SubaruDensoSh705xKlinePlan, RejectsUnknownCrossPairedAndLookalikeIdentities)
{
    const auto rejected = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"sub_ecu_denso_sh7055_04", "SH7058"},
        {"sub_ecu_denso_sh7058", "SH7055"},
        {"sub_ecu_denso_sh7055_04_future", "SH7055"},
        {"sub_ecu_denso_sh7058_can", "SH7058"},
        {"sub_ecu_denso_sh7055_02", "SH7055"},
        {"sub_ecu_denso_sh7058", "SH7058_1block"},
    });
    for (const auto& [protocol, mcu] : rejected)
    {
        SCOPED_TRACE(protocol);
        EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, protocol, mcu, std::nullopt,
                                                         kernel_for("SH7058")),
                    IsErr(ErrorKind::InvalidConfig));
    }
}

TEST(SubaruDensoSh705xKlinePlan, RejectsBadKernels)
{
    KernelImage empty = kernel_for("SH7055");
    empty.bytes.clear();
    EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, "sub_ecu_denso_sh7055_04", "SH7055",
                                                     std::nullopt, empty),
                IsErr(ErrorKind::InvalidConfig));

    // SH7058's kernel address on an SH7055 protocol.
    EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, "sub_ecu_denso_sh7055_04", "SH7055",
                                                     std::nullopt, kernel_for("SH7058")),
                IsErr(ErrorKind::InvalidConfig));

    KernelImage oversized = kernel_for("SH7058");
    oversized.bytes.assign(0x01000000, 0x00);
    EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(FlashOperation::Read, "sub_ecu_denso_sh7058", "SH7058",
                                                     std::nullopt, oversized),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoSh705xKlinePlan, WritesRequireAnExactRomSizedImage)
{
    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(operation, "sub_ecu_denso_sh7058", "SH7058", std::nullopt,
                                                         kernel_for("SH7058")),
                    IsErr(ErrorKind::InvalidConfig));
        EXPECT_THAT(build_subaru_denso_sh705x_kline_plan(operation, "sub_ecu_denso_sh7058", "SH7058",
                                                         bytes::Bytes(romsize("SH7058") - 1, 0xFF),
                                                         kernel_for("SH7058")),
                    IsErr(ErrorKind::InvalidConfig));
    }
}

TEST(SubaruDensoSh705xKlinePlan, DeviceGeometrySatisfiesTheExecutorsChunking)
{
    // The executor writes 0x200-byte chunks and commits 0x1000-byte blocks,
    // indexing the image by physical address; the plan relies on this.
    for (const std::string_view mcu : {"SH7055", "SH7058"})
    {
        const flashdev_t *device = find_flash_device(mcu);
        ASSERT_NE(device, nullptr);
        EXPECT_EQ(device->fblocks[0].start, 0U);
        std::uint32_t total = 0;
        for (unsigned i = 0; i < device->numblocks; ++i)
        {
            EXPECT_EQ(device->fblocks[i].len % 0x1000, 0U);
            total += device->fblocks[i].len;
        }
        EXPECT_EQ(total, device->romsize);
    }
}

} // namespace
} // namespace fastecu::flash
```

- [ ] **Step 2: Run to verify it fails**

Add to `src/backend/flash/ecu/BUILD.bazel`:

```python
cc_library(
    name = "subaru_denso_sh705x_kline_types",
    hdrs = ["subaru_denso_sh705x_kline_types.h"],
)

cc_library(
    name = "subaru_denso_sh705x_kline_plan",
    srcs = ["subaru_denso_sh705x_kline_plan.cpp"],
    hdrs = ["subaru_denso_sh705x_kline_plan.h"],
    deps = [
        ":subaru_denso_sh705x_kline_types",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/flash:flash_plan",
        "//src/backend/flash:flash_types",
        "//src/backend/flash:flash_validation",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_denso_sh705x_kline_plan_test",
    srcs = ["subaru_denso_sh705x_kline_plan_test.cpp"],
    deps = [
        ":subaru_denso_sh705x_kline_plan",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_kline_plan_test`
Expected: FAIL — headers missing.

- [ ] **Step 3: Write the types header**

`subaru_denso_sh705x_kline_types.h`:

```cpp
#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Wave 6b-2. Legacy connect_bootloader():269 picks the ECUTEK seed-key
// transformation when the flash method (the protocol name) ends in "_ecutek".
enum class SubaruDensoSh705xKlineSeedKey
{
    Stock,
    EcuTek,
};

struct SubaruDensoSh705xKlinePlan
{
    int initial_baud;       // 4800, execute():72
    std::uint8_t tester_id; // 0xF0, execute():73
    std::uint8_t target_id; // 0x10, execute():74
    SubaruDensoSh705xKlineSeedKey seed_key;
};

} // namespace fastecu::flash
```

- [ ] **Step 4: Register the family**

In `src/backend/flash/flash_types.h`:
- add `#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_types.h"` in alphabetical position among the `ecu/` includes;
- append to `FlashFamily` after `SubaruUnisiaJecs,`:
  ```cpp
      // Step 5 tail, wave 6b-2.
      SubaruDensoSh705xKline,
  ```
- append `SubaruDensoSh705xKlinePlan` as the last `FamilyPlan` alternative (after `SubaruUnisiaJecsPlan`);
- add after the `SubaruUnisiaJecsPlan` traits specialization:
  ```cpp
  template <> struct FamilyTraits<SubaruDensoSh705xKlinePlan>
  {
      static constexpr FlashFamily family = FlashFamily::SubaruDensoSh705xKline;
      static constexpr TransportKind transport = TransportKind::Kline;
  };
  ```
  Leave `family_requires_kernel_v` at its default (`true`): this family uploads a kernel.

In `src/backend/flash/flash_plan.cpp` `experimental_family_id()` add:
```cpp
    case FlashFamily::SubaruDensoSh705xKline:
        return "SubaruDensoSh705xKline";
```
In `src/backend/flash/testing/flash_printers.h` add:
```cpp
    case FlashFamily::SubaruDensoSh705xKline:
        *os << "SubaruDensoSh705xKline";
        return;
```
In `src/backend/flash/flash_validation_test.cpp` change `std::array<FamilyCase, 25>` to `26` in both places and append:
```cpp
        {FlashFamily::SubaruDensoSh705xKline, TransportKind::Kline,
         SubaruDensoSh705xKlinePlan{.initial_baud = 4800,
                                    .tester_id = 0xF0,
                                    .target_id = 0x10,
                                    .seed_key = SubaruDensoSh705xKlineSeedKey::Stock},
         "SubaruDensoSh705xKline"},
```
In `src/backend/flash/BUILD.bazel`, add `"//src/backend/flash/ecu:subaru_denso_sh705x_kline_types"` to the `flash_types` deps next to `subaru_unisia_jecs_types`.

- [ ] **Step 5: Write the plan header and builder**

`subaru_denso_sh705x_kline_plan.h`:

```cpp
#pragma once

#include <optional>
#include <string_view>

#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> build_subaru_denso_sh705x_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                       std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                       KernelImage kernel);
Status validate_subaru_denso_sh705x_kline_plan(const FlashPlan& plan);
} // namespace fastecu::flash
```

`subaru_denso_sh705x_kline_plan.cpp`:

```cpp
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"

#include <array>
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

// The exact protocols the legacy MainWindow branches reached:
// startsWith("sub_ecu_denso_sh7055_04") (three cfg entries today) and the
// three named sh7058 protocols. Listing them closes the prefix to lookalikes.
struct Variant
{
    std::string_view protocol;
    std::string_view mcu;
    SubaruDensoSh705xKlineSeedKey seed_key;
    // cfg read=no, test_write=yes, write=no: enforced here, not only by the UI.
    bool test_write_only;
    // cfg <kernel_addr>.
    std::uint32_t kernel_address;
};

constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_denso_sh7055_04", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock, false, 0xFFFF6004},
    {"sub_ecu_denso_sh7055_04_ecutek", "SH7055", SubaruDensoSh705xKlineSeedKey::EcuTek, false, 0xFFFF6004},
    {"sub_ecu_denso_sh7055_04_cobb", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock, true, 0xFFFF6004},
    {"sub_ecu_denso_sh7058", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock, false, 0xFFFF3000},
    {"sub_ecu_denso_sh7058_ecutek", "SH7058", SubaruDensoSh705xKlineSeedKey::EcuTek, false, 0xFFFF3000},
    {"sub_ecu_denso_sh7058_cobb", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock, true, 0xFFFF3000},
});

constexpr std::uint32_t kCommitBlockSize = 0x1000; // flash_block() flashblocksize
constexpr std::uint64_t kMaxWireLength = 0x00FFFFFF; // send_sid_34_request_upload() 24-bit length

Result<const Variant *> find_variant(std::string_view protocol, std::string_view mcu)
{
    for (const Variant& variant : kVariants)
    {
        if (variant.protocol == protocol)
        {
            if (variant.mcu != mcu)
            {
                return fail(InvalidConfig, std::format("{} requires MCU {}, not {}", protocol, variant.mcu, mcu));
            }
            return &variant;
        }
    }
    return fail(InvalidConfig, std::format("Unsupported Denso SH705x K-Line protocol: {}", protocol));
}

Status validate_operation(const Variant& variant, FlashOperation operation)
{
    if (variant.test_write_only && operation != FlashOperation::TestWrite)
    {
        return fail(Unsupported, std::format("{} supports test write only", variant.protocol));
    }
    return {};
}

Status validate_kernel(const Variant& variant, const KernelImage& kernel)
{
    if (kernel.bytes.empty())
    {
        return fail(InvalidConfig, "Denso SH705x K-Line kernel is empty");
    }
    // upload_kernel(): +2 bytes, padded to 4 -- the encrypted length must fit
    // send_sid_34_request_upload()'s 24-bit length field.
    const std::uint64_t padded = (static_cast<std::uint64_t>(kernel.bytes.size()) + 2 + 3) & ~3ULL;
    if (padded > kMaxWireLength)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line kernel exceeds the 24-bit upload length");
    }
    if (kernel.load_address != variant.kernel_address)
    {
        return fail(InvalidConfig, std::format("Denso SH705x K-Line kernel address must be 0x{:08X}",
                                               variant.kernel_address));
    }
    return {};
}

Status validate_geometry(const flashdev_t& device)
{
    // Correction (wave 6b-2): flash_block() loops `remain -= 0x200` and
    // commits at 0x1000 boundaries, and reflash_block() indexes the image by
    // physical address from fblocks[0]. Reject a table that breaks either.
    if (device.numblocks == 0 || device.fblocks[0].start != 0)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line flash blocks must start at address 0");
    }
    std::uint64_t total = 0;
    for (unsigned i = 0; i < device.numblocks; ++i)
    {
        if (device.fblocks[i].len % kCommitBlockSize != 0)
        {
            return fail(InvalidConfig, "Denso SH705x K-Line flash block is not a multiple of 0x1000");
        }
        total += device.fblocks[i].len;
    }
    if (total != device.romsize)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line flash blocks do not cover the ROM");
    }
    return {};
}

Status validate_image(FlashOperation operation, const std::optional<bytes::Bytes>& image, std::uint32_t romsize)
{
    if (operation == FlashOperation::Read)
    {
        return {};
    }
    // Correction (wave 6b-2): legacy write_mem() indexed FullRomData unchecked.
    if (!image.has_value() || image->size() != romsize)
    {
        return fail(InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }
    return {};
}

} // namespace

Status validate_subaru_denso_sh705x_kline_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruDensoSh705xKline || plan.transport() != TransportKind::Kline)
    {
        return fail(InvalidConfig, "plan is not for Denso SH705x K-Line");
    }
    Result<const Variant *> variant = find_variant(plan.target_id(), plan.mcu_name());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status valid = validate_operation(**variant, plan.operation()); !valid.has_value())
    {
        return valid;
    }
    const auto *family = std::get_if<SubaruDensoSh705xKlinePlan>(&plan.family_plan());
    if (family == nullptr || family->initial_baud != 4800 || family->tester_id != 0xF0 ||
        family->target_id != 0x10 || family->seed_key != (*variant)->seed_key)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line wire parameters are invalid");
    }
    if (!plan.erase_regions().empty() || !plan.confirmations().empty())
    {
        return fail(InvalidConfig, "Denso SH705x K-Line plans carry no erase regions or confirmations");
    }
    if (!plan.kernel().has_value())
    {
        return fail(InvalidConfig, "Denso SH705x K-Line requires a kernel image");
    }
    if (Status valid = validate_kernel(**variant, *plan.kernel()); !valid.has_value())
    {
        return valid;
    }
    const flashdev_t *device = find_flash_device(plan.mcu_name());
    if (device == nullptr)
    {
        return fail(InvalidConfig, "Unknown MCU type");
    }
    if (Status valid = validate_geometry(*device); !valid.has_value())
    {
        return valid;
    }
    if (plan.transfer_region().start != 0 || plan.transfer_region().length != device->romsize)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line transfer region does not match the MCU");
    }
    return validate_image(plan.operation(), plan.image(), device->romsize);
}

Result<FlashPlan> build_subaru_denso_sh705x_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                       std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                       KernelImage kernel)
{
    Result<const Variant *> variant = find_variant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status valid = validate_operation(**variant, operation); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (Status valid = validate_kernel(**variant, kernel); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const flashdev_t *device = find_flash_device(mcu_type);
    if (device == nullptr)
    {
        return fail(InvalidConfig, "Unknown MCU type");
    }
    if (Status valid = validate_geometry(*device); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (Status valid = validate_image(operation, image, device->romsize); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh705xKline,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = MemoryRegion{0, device->romsize},
        .erase_regions = {},
        .image = operation == FlashOperation::Read ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = SubaruDensoSh705xKlinePlan{.initial_baud = 4800,
                                                  .tester_id = 0xF0,
                                                  .target_id = 0x10,
                                                  .seed_key = (*variant)->seed_key},
        .confirmations = {},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_kline_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
```

Register `"subaru_denso_sh705x_kline_plan"` and `"subaru_denso_sh705x_kline_types"` in `bazel/portable_targets.bzl` (`"src/backend/flash/ecu"` list, after the `subaru_denso_sh705x_densocan_*` entries).

- [ ] **Step 6: Run to verify it passes**

Run:
```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_kline_plan_test \
  //src/backend/flash:flash_validation_test //src/backend/flash:flash_types_test
bazel build --config=release //:portable_closure
```
Expected: PASS.

- [ ] **Step 7: Mutation check the Cobb gate**

Change `variant.test_write_only && operation != FlashOperation::TestWrite` to `false`. Run the plan test — expect `CobbIsTestWriteOnly` to FAIL. Restore; `git diff --exit-code -- src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.cpp`.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/ecu/subaru_denso_sh705x_kline_types.h src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.* \
  src/backend/flash/ecu/BUILD.bazel src/backend/flash/flash_types.h src/backend/flash/flash_plan.cpp \
  src/backend/flash/flash_validation_test.cpp src/backend/flash/testing/flash_printers.h \
  src/backend/flash/BUILD.bazel bazel/portable_targets.bzl
git commit -m "feat(flash): add the Denso SH705x K-Line plan"
```

### Task 5: Executor skeleton — setup, reset seam, framing, balanced kernel

**Files:**
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h`, `.cpp`, `_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: Task 4's plan API; Task 2's `denso_sh705x_kline_*` helpers; Task 1's `reset_connection()` and `lifecycle_calls_`.
- Produces:
  ```cpp
  class SubaruDensoSh705xKlineExecutor final : public IKlineFlashExecutor {
    public:
      Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
      Status before_transport_configure(IKlineFlashTransport& transport, IClock& clock,
                                        const ICancellationToken& cancellation) const override;
      Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                           const ICancellationToken& cancellation, IEventSink& events) override;
  };
  // upload_kernel():361-387, exposed for a golden-vector test.
  bytes::Bytes denso_sh705x_kline_balanced_kernel(bytes::ByteView kernel);
  ```

- [ ] **Step 1: Write the failing tests**

`subaru_denso_sh705x_kline_executor_test.cpp` — start the file with the shared fixture that Tasks 6-8 extend:

```cpp
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"

#include <array>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
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
using bytes::composeBe;
using bytes::composeBeWithChecksum;
using bytes::u24;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using namespace bytes::literals;
using namespace std::chrono_literals;

// ---- Wire transcription, independent of production helpers -------------

// Kernel frame: BE EF, u16 length (opcode + payload), opcode, payload, sum8.
bytes::Bytes beef(std::uint8_t opcode, bytes::ByteView payload = {})
{
    return composeBeWithChecksum(bytes::sum8, std::uint16_t{0xBEEF}, std::uint16_t(payload.size() + 1),
                                 bytes::Byte(opcode), payload);
}
// A positive kernel reply: BE EF, length, opcode|0x40, data, sum8.
bytes::Bytes beef_reply(std::uint8_t opcode, bytes::ByteView data = {})
{
    return beef(static_cast<std::uint8_t>(opcode | 0x40), data);
}
// SSM request tester 0xF0 -> target 0x10: 80 10 F0 len payload sum8.
bytes::Bytes ssm(bytes::ByteView payload)
{
    return composeBeWithChecksum(bytes::sum8, 0x80_b, 0x10_b, 0xF0_b, bytes::Byte(payload.size()), payload);
}
// SSM reply target -> tester: 80 F0 10 len payload sum8.
bytes::Bytes ssm_reply(bytes::ByteView payload)
{
    return composeBeWithChecksum(bytes::sum8, 0x80_b, 0xF0_b, 0x10_b, bytes::Byte(payload.size()), payload);
}

constexpr auto kKeyTable = std::to_array<std::uint16_t>({0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F,
                                                         0x3608, 0xAFB8, 0x503D, 0xDBA3, 0x9D34, 0x3563, 0x6B70,
                                                         0x6E74, 0x88F0});
constexpr auto kEncryptTable = std::to_array<std::uint16_t>({0x7856, 0xCE22, 0xF513, 0x6E86});

const bytes::Bytes kKernelIdRequest{0xBE, 0xEF, 0x00, 0x01, 0x01, 0xAF};
const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};
const bytes::Bytes kKernelBytes{0xAA, 0xBB, 0xCC, 0xDD};
// upload_kernel() transform of kKernelBytes, worked by hand in Task 5.
const bytes::Bytes kBalancedKernel{0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x8D, 0xC8};

bytes::Bytes kernel_id_reply()
{
    return beef_reply(0x01, bytes::Bytes{'S', 'S', 'M', 'K'});
}

KernelImage kernel_for(std::string_view mcu)
{
    return KernelImage{.id = "k", .load_address = mcu == "SH7055" ? 0xFFFF6004U : 0xFFFF3000U, .bytes = kKernelBytes};
}

FlashPlan make_plan(FlashOperation operation, std::string_view protocol = "sub_ecu_denso_sh7055_04",
                    std::string_view mcu = "SH7055", std::optional<bytes::Bytes> image = std::nullopt)
{
    if (operation != FlashOperation::Read && !image.has_value())
    {
        image = bytes::Bytes(find_flash_device(mcu)->romsize, 0xFF);
    }
    auto plan = build_subaru_denso_sh705x_kline_plan(operation, protocol, mcu, std::move(image), kernel_for(mcu));
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

struct Harness
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    Result<FlashExecutionResult> run(const FlashPlan& plan)
    {
        return executor.execute(plan, transport, clock, cancellation, events);
    }
};

TEST(SubaruDensoSh705xKlineExecutor, BalancedKernelMatchesHandWorkedVectors)
{
    // AA BB CC DD +00 00 -> pad to 8 -> drop 2 -> AA BB CC DD 00 00.
    // Words (bytes past the end read as zero): AABBCCDD, 00000000.
    // u16 sum = 0xCCDD; 0x5AA5 - 0xCCDD = 0x8DC8 (mod 2^16).
    EXPECT_EQ(denso_sh705x_kline_balanced_kernel(kKernelBytes), kBalancedKernel);
    // 01 02 03 +00 00 -> 01 02 03 00 00 00 00 00 -> drop 2 -> 6 bytes.
    // u16 sum = 0x0300; balance = 0x57A5.
    EXPECT_EQ(denso_sh705x_kline_balanced_kernel(bytes::Bytes{0x01, 0x02, 0x03}),
              (bytes::Bytes{0x01, 0x02, 0x03, 0x00, 0x00, 0x00, 0x57, 0xA5}));
}

TEST(SubaruDensoSh705xKlineExecutor, BalancedKernelWordSumIsAlways5AA5)
{
    for (std::size_t size = 1; size <= 9; ++size)
    {
        SCOPED_TRACE(size);
        bytes::Bytes kernel(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            kernel[i] = static_cast<bytes::Byte>(0x31 * (i + 1));
        }
        const bytes::Bytes out = denso_sh705x_kline_balanced_kernel(kernel);
        ASSERT_EQ(out.size() % 4, 0U);
        std::uint16_t sum = 0;
        for (std::size_t i = 0; i < out.size(); i += 4)
        {
            sum = static_cast<std::uint16_t>(sum + bytes::readU32Be(out, i));
        }
        EXPECT_EQ(sum, 0x5AA5);
    }
}

TEST(SubaruDensoSh705xKlineExecutor, TransportSetupIsNonIso14230At4800)
{
    SubaruDensoSh705xKlineExecutor executor;
    const auto config = executor.transport_setup(make_plan(FlashOperation::Read));
    ASSERT_THAT(config, IsOk());
    EXPECT_EQ(config->baud, 4800);
    EXPECT_FALSE(config->iso14230);
    EXPECT_EQ(config->tester_id, 0xF0);
    EXPECT_EQ(config->target_id, 0x10);
    EXPECT_EQ(config->parity, KlineParity::None);
}

TEST(SubaruDensoSh705xKlineExecutor, BoundAttemptResetsBeforeConfigure)
{
    auto transport = std::make_unique<ScriptedKlineFlashTransport>();
    auto *observed = transport.get();
    observed->set_baud_result_ = fail(ErrorKind::Disconnected, "stop after lifecycle");
    auto attempt = bind_flash_attempt(make_plan(FlashOperation::Read), std::make_unique<SubaruDensoSh705xKlineExecutor>(),
                                      std::move(transport));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    EXPECT_THAT(attempt->run(clock, cancellation, events), IsErr(ErrorKind::Disconnected));
    // execute():67 reset_connection() precedes every setter and open_serial_port().
    EXPECT_THAT(observed->lifecycle_calls_, ::testing::ElementsAre("reset_connection", "configure", "open", "close"));
}

TEST(SubaruDensoSh705xKlineExecutor, CancellationAroundResetStopsBeforeConfigure)
{
    for (const std::size_t check : {1U, 2U})
    {
        SCOPED_TRACE(check);
        ScriptedKlineFlashTransport transport;
        FakeClock clock;
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(check);
        SubaruDensoSh705xKlineExecutor executor;

        EXPECT_THAT(executor.before_transport_configure(transport, clock, cancellation), IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(transport.reset_call_count_, check == 1 ? 0 : 1);
    }
}

TEST(SubaruDensoSh705xKlineExecutor, ResetFailurePropagates)
{
    ScriptedKlineFlashTransport transport;
    transport.reset_result_ = fail(ErrorKind::Disconnected, "no adapter");
    FakeClock clock;
    FakeCancellationToken cancellation;
    SubaruDensoSh705xKlineExecutor executor;

    EXPECT_THAT(executor.before_transport_configure(transport, clock, cancellation), IsErr(ErrorKind::Disconnected));
}

TEST(SubaruDensoSh705xKlineExecutor, RejectsAForeignPlanBeforeIo)
{
    Harness h;
    FlashPlanFields fields{.operation = FlashOperation::Read,
                           .family = FlashFamily::SubaruUnisiaJecs,
                           .transport = TransportKind::Kline,
                           .target_id = "sub_ecu_unisia_jecs_m3779x",
                           .mcu_name = "M3779x",
                           .transfer_region = {0, 0x10000},
                           .erase_regions = {},
                           .image = std::nullopt,
                           .kernel = std::nullopt,
                           .family_plan = SubaruUnisiaJecsPlan{.initial_baud = 1953, .even_parity = true},
                           .confirmations = {}};
    auto foreign = validate_and_build(std::move(fields));
    ASSERT_THAT(foreign, IsOk());

    EXPECT_THAT(h.executor.transport_setup(*foreign), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(h.run(*foreign), IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(h.transport.writesConsumed(), 0U);
}

} // namespace
} // namespace fastecu::flash
```

Check `FakeCancellationToken::cancel_on_check` semantics in `src/backend/ports/testing/fake_cancellation_token.h` before relying on the `check` numbering; adjust only the numbers, not the intent (first check before reset, second after). If `SubaruUnisiaJecsPlan` has other required fields, copy the literal from `flash_validation_test.cpp`'s `family_cases()`.

Add to `src/backend/flash/ecu/BUILD.bazel`:

```python
cc_library(
    name = "subaru_denso_sh705x_kline_executor",
    srcs = ["subaru_denso_sh705x_kline_executor.cpp"],
    hdrs = ["subaru_denso_sh705x_kline_executor.h"],
    deps = [
        ":denso_sh705x_kline_common",
        ":subaru_denso_sh705x_kline_plan",
        "//src/algorithms/checksum",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/ssm",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/flash:flash_executor",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "subaru_denso_sh705x_kline_executor_test",
    srcs = ["subaru_denso_sh705x_kline_executor_test.cpp"],
    deps = [
        ":subaru_denso_sh705x_kline_executor",
        ":subaru_denso_sh705x_kline_plan",
        "//src/algorithms/checksum",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/ssm",
        "//src/backend/definitions:models",
        "//src/backend/flash:flash_device_lookup",
        "//src/backend/flash:flash_validation",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:recording_event_sink",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_kline_executor_test`
Expected: FAIL — executor header missing.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable replacement for FlashEcuSubaruDensoSH705xKlineOperation (wave 6b-2).
class SubaruDensoSh705xKlineExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Status before_transport_configure(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};

// upload_kernel(): append 00 00, pad to a multiple of four, drop the last two
// bytes, then append the big-endian 16-bit balance 0x5AA5 - (sum of the
// big-endian 32-bit words, mod 2^16). Legacy summed words with QByteArray::at()
// two bytes past the end, where the truncated padding still reads as zero;
// this treats those bytes as zero explicitly. Exposed for its golden test.
bytes::Bytes denso_sh705x_kline_balanced_kernel(bytes::ByteView kernel);

} // namespace fastecu::flash
```

- [ ] **Step 4: Write the skeleton implementation**

`subaru_denso_sh705x_kline_executor.cpp` — helpers used by every later task, plus the three public members. `execute()` is completed in Tasks 6-8; for now it validates and returns `Unsupported`:

```cpp
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"

#include <chrono>
#include <format>
#include <string>
#include <utility>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/denso_sh705x_kline_common.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::composeBeWithChecksum;
using bytes::u24;
using namespace bytes::literals;
using namespace std::chrono_literals;
using OptionalBytes = IKlineFlashTransport::OptionalBytes;

// Legacy: src/platform/desktop/common/flash/legacy/ecu/
// flash_ecu_subaru_denso_sh705x_kline_operation.{h,cpp}, deleted in wave 6b-2.
constexpr std::uint16_t kStartComm = 0xBEEF; // kernelcomms.h SUB_KERNEL_START_COMM
constexpr std::uint8_t kOpId = 0x01;
constexpr std::uint8_t kOpCrc = 0x02;
constexpr std::uint8_t kOpReadArea = 0x03;
constexpr std::uint8_t kOpProgVolt = 0x04;
constexpr std::uint8_t kOpGetMaxMsgSize = 0x05;
constexpr std::uint8_t kOpGetMaxBlockSize = 0x06;
constexpr std::uint8_t kOpFlashEnable = 0x20;
constexpr std::uint8_t kOpFlashDisable = 0x21;
constexpr std::uint8_t kOpWriteFlashBuffer = 0x22;
constexpr std::uint8_t kOpValidateFlashBuffer = 0x23;
constexpr std::uint8_t kOpCommitFlashBuffer = 0x24;
constexpr std::uint8_t kOpBlankPage = 0x25;

// Header :48-53.
constexpr std::chrono::milliseconds kReadTimeout = 2000ms;      // serial_read_timeout
constexpr std::chrono::milliseconds kShortTimeout = 200ms;      // serial_read_short_timeout
constexpr std::chrono::milliseconds kMediumTimeout = 500ms;     // serial_read_medium_timeout
constexpr std::chrono::milliseconds kLongTimeout = 800ms;       // serial_read_long_timeout
constexpr std::chrono::milliseconds kExtraLongTimeout = 3000ms; // serial_read_extra_long_timeout

constexpr int kProbeBaud = 62500;  // connect_bootloader():127, upload_kernel():463
constexpr int kSsmBaud = 4800;     // connect_bootloader():161
constexpr int kUploadBaud = 15625; // upload_kernel():354
constexpr std::chrono::milliseconds kBaudSettle = 100ms;        // connect_bootloader():128, :162
constexpr std::chrono::milliseconds kKernelIdSettle = 200ms;    // request_kernel_id() delay(200)
constexpr std::chrono::milliseconds kPostStartRoutine = 100ms;  // upload_kernel() delay(100) before 62500
constexpr std::chrono::milliseconds kCrcBlockPacing = 5ms;      // get_changed_blocks() delay(5)

constexpr std::uint32_t kReadPageSize = 0x400;    // read_mem() pagesize
constexpr std::uint32_t kWriteChunkSize = 0x200;  // flash_block() blocksize
constexpr std::uint32_t kCommitBlockSize = 0x1000; // flash_block() flashblocksize
constexpr std::uint32_t kUploadChunkBytes = 0x80; // send_sid_36_transferdata() blocksize

Status check_cancelled(const ICancellationToken& cancellation, std::string detail)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::move(detail));
    }
    return {};
}

// Every kernel request, e.g. request_kernel_id() and check_romcrc():
// BE EF, u16 length (opcode + payload), opcode, payload, sum8.
bytes::Bytes frame(std::uint8_t opcode, bytes::ByteView payload = {})
{
    return composeBeWithChecksum(bytes::sum8, kStartComm, static_cast<std::uint16_t>(payload.size() + 1),
                                 bytes::Byte(opcode), payload);
}

// Legacy kernel-reply checks index at(0), at(1) and at(4) after a length check.
bool kernel_reply_ok(bytes::ByteView received, std::uint8_t opcode, std::size_t min_size)
{
    return received.size() >= min_size && received[0] == 0xBE && received[1] == 0xEF &&
           received[4] == static_cast<bytes::Byte>(opcode | 0x40U);
}

// write -> [settle] -> read, cancellation-checked at every boundary.
Result<OptionalBytes> exchange(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               bytes::ByteView request, std::chrono::milliseconds settle,
                               std::chrono::milliseconds timeout)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before write"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    Result<std::size_t> written = transport.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return fail(ErrorKind::Disconnected, "short K-Line write");
    }
    if (settle > 0ms)
    {
        if (Status slept = clock.sleep(settle, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    Result<OptionalBytes> received = transport.read(timeout, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled after read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    return received;
}

// Like exchange(), but no frame at all is a Timeout -- legacy treated an
// empty reply as "No valid response from ECU" and stopped.
Result<bytes::Bytes> required_exchange(IKlineFlashTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation, bytes::ByteView request,
                                       std::chrono::milliseconds settle, std::chrono::milliseconds timeout,
                                       std::string_view what)
{
    Result<OptionalBytes> received = exchange(transport, clock, cancellation, request, settle, timeout);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, std::format("no response from ECU during {}", what));
    }
    return std::move(**received);
}

Status change_baud(IKlineFlashTransport& transport, const ICancellationToken& cancellation, int baud)
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled before changing baud"); !cancelled.has_value())
    {
        return cancelled;
    }
    return transport.setBaud(baud);
}

Status sleep_for(IClock& clock, const ICancellationToken& cancellation, std::chrono::milliseconds duration)
{
    return clock.sleep(duration, cancellation);
}

} // namespace

bytes::Bytes denso_sh705x_kline_balanced_kernel(bytes::ByteView kernel)
{
    // upload_kernel():361-387.
    bytes::Bytes out(kernel.begin(), kernel.end());
    out.push_back(0x00);
    out.push_back(0x00);
    out.resize((out.size() + 3) & ~std::size_t{3}, 0x00);
    out.resize(out.size() - 2);
    const auto at = [&out](std::size_t index) -> std::uint32_t { return index < out.size() ? out[index] : 0U; };
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < out.size(); i += 4)
    {
        sum = static_cast<std::uint16_t>(sum + ((at(i) << 24) | (at(i + 1) << 16) | (at(i + 2) << 8) | at(i + 3)));
    }
    const auto balance = static_cast<std::uint16_t>(0x5AA5 - sum);
    out.push_back(static_cast<bytes::Byte>(balance >> 8));
    out.push_back(static_cast<bytes::Byte>(balance & 0xFF));
    return out;
}

Result<KlineConfig> SubaruDensoSh705xKlineExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::SubaruDensoSh705xKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return non_iso14230_kline_config_from(std::get<SubaruDensoSh705xKlinePlan>(plan.family_plan()));
}

Status SubaruDensoSh705xKlineExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                  const ICancellationToken& cancellation) const
{
    // execute():67 -- serial->reset_connection() before every setter.
    if (Status cancelled = check_cancelled(cancellation, "cancelled before K-Line reset"); !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status reset = transport.reset_connection(); !reset.has_value())
    {
        return reset;
    }
    return check_cancelled(cancellation, "cancelled after K-Line reset");
}

Result<FlashExecutionResult> SubaruDensoSh705xKlineExecutor::execute(const FlashPlan& plan, IKlineFlashTransport&,
                                                                     IClock&, const ICancellationToken&, IEventSink&)
{
    if (Status match = check_family(plan, FlashFamily::SubaruDensoSh705xKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return fail(ErrorKind::Unsupported, "Denso SH705x K-Line execute() is completed in wave 6b-2 tasks 6-8");
}

} // namespace fastecu::flash
```

`BoundAttemptResetsBeforeConfigure` reaches `execute()`, which returns `Unsupported` at this stage; change that test's expectation to `IsErr(ErrorKind::Unsupported)` now, and back to `Disconnected` in Task 6 Step 4 when the first `setBaud` runs. Unused helpers will warn under clang-tidy until Tasks 6-8 use them; that is expected mid-PR.

Register `"subaru_denso_sh705x_kline_executor"` in `bazel/portable_targets.bzl` after the plan entry.

- [ ] **Step 5: Run to verify it passes**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_kline_executor_test`
Expected: PASS (7 tests).

- [ ] **Step 6: Mutation check the reset seam**

Delete the `transport.reset_connection()` call. Expect `BoundAttemptResetsBeforeConfigure` and `CancellationAroundResetStopsBeforeConfigure` to FAIL. Restore; `git diff --exit-code` on the `.cpp`.

- [ ] **Step 7: Commit**

```bash
git add src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.* src/backend/flash/ecu/BUILD.bazel bazel/portable_targets.bzl
git commit -m "feat(flash): add the Denso SH705x K-Line executor skeleton"
```

### Task 6: Kernel probe, bootloader handshake, kernel upload

**Files:**
- Modify: `src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.cpp`
- Test: `src/backend/flash/ecu/subaru_denso_sh705x_kline_executor_test.cpp`

**Interfaces:**
- Consumes: Task 5 helpers (`frame`, `kernel_reply_ok`, `exchange`, `required_exchange`, `change_baud`, `sleep_for`), `denso_sh705x_kline_{stock,ecutek}_seed_key`, `denso_sh705x_kline_encrypt_payload`, `denso_sh705x_kline_balanced_kernel`.
- Produces (anonymous namespace): `Result<bool> probe_kernel(...)`, `Result<std::string> connect_bootloader(...)` (returns ECU ID hex), `Status upload_kernel(...)`, and `Result<std::optional<std::string>> start_session(...)` which runs probe → (handshake → upload) and returns the ECU ID only when the handshake ran. `execute()` now calls `start_session()` and then returns `Unsupported` for the operation-specific tail until Tasks 7-8.

- [ ] **Step 1: Write the failing tests**

Add script helpers and tests to the executor test file (inside the anonymous namespace, below the fixture):

```cpp
// connect_bootloader():127-157 -- kernel probe at 62500.
void script_probe_dead(ScriptedKlineFlashTransport& t)
{
    auto s = t.section("probe");
    t.expectWrite(kKernelIdRequest);
    t.queue_no_frame();
}
void script_probe_alive(ScriptedKlineFlashTransport& t)
{
    auto s = t.section("probe");
    t.exchange(kKernelIdRequest, kernel_id_reply());
}

bytes::Bytes stock_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kKeyTable, SsmProtocol::kIndexTransformationStock);
}
bytes::Bytes ecutek_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kKeyTable, SsmProtocol::kIndexTransformationEcutek);
}

// connect_bootloader():161-318 -- SSM handshake at 4800.
void script_handshake(ScriptedKlineFlashTransport& t, bytes::ByteView key)
{
    auto s = t.section("handshake");
    t.exchange(ssm(bytes::Bytes{0xBF}),
               ssm_reply(bytes::Bytes{0xFF, 0x00, 0x00, 0x00, 0x41, 0x42, 0x43, 0x44, 0x45}));
    t.exchange(ssm(bytes::Bytes{0x81}), ssm_reply(bytes::Bytes{0xC1}));
    t.exchange(ssm(bytes::Bytes{0x83, 0x00}), ssm_reply(bytes::Bytes{0xC3}));
    t.exchange(ssm(bytes::Bytes{0x27, 0x01}), ssm_reply(composeBe(0x67_b, 0x01_b, kSeed)));
    t.exchange(ssm(composeBe(0x27_b, 0x02_b, key)), ssm_reply(bytes::Bytes{0x67, 0x02}));
    t.exchange(ssm(bytes::Bytes{0x10, 0x85, 0x02}), ssm_reply(bytes::Bytes{0x50}));
}

// upload_kernel():354-500 -- 34 / 36 / 31 at 15625, then kernel ID at 62500.
void script_upload(ScriptedKlineFlashTransport& t, std::uint32_t address)
{
    auto s = t.section("upload");
    const bytes::Bytes encrypted =
        SsmProtocol::calculatePayload(kBalancedKernel, 8, kEncryptTable, SsmProtocol::kIndexTransformationStock);
    t.exchange(ssm(composeBe(0x34_b, u24(address), 0x04_b, u24(8))), ssm_reply(bytes::Bytes{0x74}));
    t.exchange(ssm(composeBe(0x36_b, u24(address), encrypted)), ssm_reply(bytes::Bytes{0x76}));
    t.exchange(ssm(bytes::Bytes{0x31, 0x01, 0x01}), ssm_reply(bytes::Bytes{0x71}));
    t.exchange(kKernelIdRequest, kernel_id_reply());
}

void script_session(ScriptedKlineFlashTransport& t, bytes::ByteView key = {}, std::uint32_t address = 0xFFFF6004)
{
    script_probe_dead(t);
    script_handshake(t, key.empty() ? stock_key(kSeed) : bytes::Bytes(key.begin(), key.end()));
    script_upload(t, address);
}
```

Tests — each ends at the operation tail, which returns `Unsupported` until Task 7; the assertions are on the transcript:

```cpp
TEST(SubaruDensoSh705xKlineExecutor, FullSessionIsByteExactWithLegacyBaudsAndTimeouts)
{
    Harness h;
    script_session(h.transport);

    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Unsupported));
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(h.transport.baud_calls_, ::testing::ElementsAre(62500, 4800, 15625, 62500));
    // probe 800, BF..10 six x 2000, 34 3000, 36 2000, 31 3000, kernel ID 800.
    EXPECT_THAT(h.transport.read_timeouts_,
                ::testing::ElementsAre(800ms, 2000ms, 2000ms, 2000ms, 2000ms, 2000ms, 2000ms, 3000ms, 2000ms, 3000ms,
                                       800ms));
}

TEST(SubaruDensoSh705xKlineExecutor, EcutekProtocolSendsTheEcutekKey)
{
    Harness h;
    script_session(h.transport, ecutek_key(kSeed));
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read, "sub_ecu_denso_sh7055_04_ecutek")),
                IsErr(ErrorKind::Unsupported));
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, Sh7058UploadsToItsOwnKernelAddress)
{
    Harness h;
    script_session(h.transport, {}, 0xFFFF3000);
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read, "sub_ecu_denso_sh7058", "SH7058")),
                IsErr(ErrorKind::Unsupported));
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, LiveKernelSkipsHandshakeAndUpload)
{
    Harness h;
    script_probe_alive(h.transport);
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Unsupported));
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_THAT(h.transport.baud_calls_, ::testing::ElementsAre(62500));
}

TEST(SubaruDensoSh705xKlineExecutor, WrongProbeReplyFallsThroughToHandshake)
{
    Harness h;
    {
        auto s = h.transport.section("probe");
        h.transport.exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00});
    }
    script_handshake(h.transport, stock_key(kSeed));
    script_upload(h.transport, 0xFFFF6004);
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Unsupported));
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, ShortEcuIdReplyIsRejectedBeforeSlicing)
{
    // Correction: legacy removed 8 bytes after checking only `> 4`.
    Harness h;
    script_probe_dead(h.transport);
    h.transport.exchange(ssm(bytes::Bytes{0xBF}), ssm_reply(bytes::Bytes{0xFF, 0x00}));
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, EachNegativeHandshakeReplyStopsTheSession)
{
    struct Case
    {
        int step; // 0 = BF ... 5 = 10
        bytes::Bytes reply_payload;
    };
    const std::vector<Case> cases{{0, {0x7F, 0xBF}}, {1, {0x7F, 0x81}},       {2, {0x7F, 0x83}},
                                  {3, {0x67, 0x02, 0x11, 0x22, 0x33, 0x44}}, {4, {0x67, 0x01}},
                                  {5, {0x7F, 0x10}}};
    const std::vector<bytes::Bytes> requests{ssm(bytes::Bytes{0xBF}),       ssm(bytes::Bytes{0x81}),
                                             ssm(bytes::Bytes{0x83, 0x00}), ssm(bytes::Bytes{0x27, 0x01}),
                                             ssm(composeBe(0x27_b, 0x02_b, stock_key(kSeed))),
                                             ssm(bytes::Bytes{0x10, 0x85, 0x02})};
    const std::vector<bytes::Bytes> good{
        ssm_reply(bytes::Bytes{0xFF, 0x00, 0x00, 0x00, 0x41, 0x42, 0x43, 0x44, 0x45}),
        ssm_reply(bytes::Bytes{0xC1}),
        ssm_reply(bytes::Bytes{0xC3}),
        ssm_reply(composeBe(0x67_b, 0x01_b, kSeed)),
        ssm_reply(bytes::Bytes{0x67, 0x02}),
        ssm_reply(bytes::Bytes{0x50})};
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.step);
        Harness h;
        script_probe_dead(h.transport);
        for (int i = 0; i < c.step; ++i)
        {
            h.transport.exchange(requests[i], good[i]);
        }
        h.transport.exchange(requests[c.step], ssm_reply(c.reply_payload));
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed()); // nothing sent after the bad reply
    }
}

TEST(SubaruDensoSh705xKlineExecutor, SilentHandshakeStepIsATimeout)
{
    Harness h;
    script_probe_dead(h.transport);
    h.transport.expectWrite(ssm(bytes::Bytes{0xBF}));
    h.transport.queue_no_frame();
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Timeout));
}

TEST(SubaruDensoSh705xKlineExecutor, FailedUploadBaudChangeStops)
{
    // setBaud results are shared across calls; fail from the third call on.
    struct FailThirdBaud final : ScriptedKlineFlashTransport
    {
        using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;
        Status setBaud(int baud) override
        {
            baud_calls_.push_back(baud);
            return baud_calls_.size() >= 3 ? fail(ErrorKind::Disconnected, "baud") : Status{};
        }
    };
    FailThirdBaud t{ScriptedTransportInitialState::Open};
    script_probe_dead(t);
    script_handshake(t, stock_key(kSeed));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;
    EXPECT_THAT(executor.execute(make_plan(FlashOperation::Read), t, clock, cancellation, events),
                IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(t.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, FinalBaudChangeFailureIsNowChecked)
{
    // Correction: legacy ignored upload_kernel()'s change_port_speed("62500").
    struct FailFourthBaud final : ScriptedKlineFlashTransport
    {
        using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;
        Status setBaud(int baud) override
        {
            baud_calls_.push_back(baud);
            return baud_calls_.size() == 4 ? fail(ErrorKind::Disconnected, "baud") : Status{};
        }
    };
    FailFourthBaud t{ScriptedTransportInitialState::Open};
    script_probe_dead(t);
    script_handshake(t, stock_key(kSeed));
    {
        auto s = t.section("upload without kernel ID");
        const bytes::Bytes encrypted =
            SsmProtocol::calculatePayload(kBalancedKernel, 8, kEncryptTable, SsmProtocol::kIndexTransformationStock);
        t.exchange(ssm(composeBe(0x34_b, u24(0xFFFF6004), 0x04_b, u24(8))), ssm_reply(bytes::Bytes{0x74}));
        t.exchange(ssm(composeBe(0x36_b, u24(0xFFFF6004), encrypted)), ssm_reply(bytes::Bytes{0x76}));
        t.exchange(ssm(bytes::Bytes{0x31, 0x01, 0x01}), ssm_reply(bytes::Bytes{0x71}));
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;
    EXPECT_THAT(executor.execute(make_plan(FlashOperation::Read), t, clock, cancellation, events),
                IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(t.scriptConsumed()); // no kernel-ID request after the failed baud change
}

TEST(SubaruDensoSh705xKlineExecutor, DeadKernelAfterUploadIsABadResponse)
{
    Harness h;
    script_probe_dead(h.transport);
    script_handshake(h.transport, stock_key(kSeed));
    {
        auto s = h.transport.section("upload");
        const bytes::Bytes encrypted =
            SsmProtocol::calculatePayload(kBalancedKernel, 8, kEncryptTable, SsmProtocol::kIndexTransformationStock);
        h.transport.exchange(ssm(composeBe(0x34_b, u24(0xFFFF6004), 0x04_b, u24(8))), ssm_reply(bytes::Bytes{0x74}));
        h.transport.exchange(ssm(composeBe(0x36_b, u24(0xFFFF6004), encrypted)), ssm_reply(bytes::Bytes{0x76}));
        h.transport.exchange(ssm(bytes::Bytes{0x31, 0x01, 0x01}), ssm_reply(bytes::Bytes{0x71}));
        h.transport.exchange(kKernelIdRequest, bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00});
    }
    EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
}

// The session writes 11 frames: probe 1, handshake 6, 34/36/31 3, kernel ID 1.
constexpr std::size_t kSessionWrites = 11;

TEST(SubaruDensoSh705xKlineExecutor, CancellationBeforeAnySessionWriteSendsNothingFurther)
{
    // Keyed on writes, not check numbers, so it stays valid as tasks 7-8
    // extend execute(): cancelling once k frames are out must send no more.
    for (std::size_t k = 0; k < kSessionWrites; ++k)
    {
        SCOPED_TRACE(k);
        Harness h;
        script_session(h.transport);
        h.cancellation.set_predicate([&h, k] { return h.transport.writesConsumed() >= k; });
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(h.transport.writesConsumed(), k);
    }
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_kline_executor_test`
Expected: the new tests FAIL (execute() returns `Unsupported` before any write; `scriptConsumed()` is false).

- [ ] **Step 3: Implement the session phases**

Add to the anonymous namespace in the executor `.cpp`, after `sleep_for`. Copy each `events.log(...)` string verbatim from the cited legacy `emit LOG_I`/`LOG_E` line; the strings below are the legacy text.

```cpp
std::string ecu_id_hex(bytes::ByteView id)
{
    std::string out;
    for (const bytes::Byte byte : id)
    {
        out += std::format("{:02X}", byte);
    }
    return out;
}

// request_kernel_id(): write, delay(200), read(serial_read_long_timeout).
Result<OptionalBytes> request_kernel_id(IKlineFlashTransport& transport, IClock& clock,
                                        const ICancellationToken& cancellation)
{
    return exchange(transport, clock, cancellation, frame(kOpId), kKernelIdSettle, kLongTimeout);
}

// connect_bootloader():127-157. A live kernel is the only success; any other
// reply is the normal "not running" case and falls through, as in legacy.
Result<bool> probe_kernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                          IEventSink& events)
{
    if (Status baud = change_baud(transport, cancellation, kProbeBaud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    if (Status slept = sleep_for(clock, cancellation, kBaudSettle); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    events.log(LogLevel::Info, "Checking if kernel is already running...");
    events.log(LogLevel::Info, "Requesting kernel ID");
    Result<OptionalBytes> received = request_kernel_id(transport, clock, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value() || (**received).size() <= 4)
    {
        events.log(LogLevel::Error, "No valid response from ECU");
        return false;
    }
    const bytes::Bytes& reply = **received;
    if (!kernel_reply_ok(reply, kOpId, 5))
    {
        events.log(LogLevel::Error, "Wrong response from ECU");
        return false;
    }
    events.log(LogLevel::Info, "Kernel ID: " + std::string(reply.begin() + 5, reply.end() - 1));
    return true;
}

Result<bytes::Bytes> ssm_exchange(IKlineFlashTransport& transport, IClock& clock,
                                  const ICancellationToken& cancellation, const SubaruDensoSh705xKlinePlan& plan,
                                  bytes::ByteView payload, std::chrono::milliseconds timeout, std::string_view what)
{
    return required_exchange(transport, clock, cancellation,
                             SsmProtocol::addHeader(payload, plan.tester_id, plan.target_id), 0ms, timeout, what);
}

Status require_ssm(const bytes::Bytes& reply, std::size_t min_size, std::uint8_t byte4,
                   std::optional<std::uint8_t> byte5, IEventSink& events)
{
    if (reply.size() < min_size)
    {
        events.log(LogLevel::Error, "No valid response from ECU");
        return fail(ErrorKind::BadResponse, "No valid response from ECU");
    }
    if (reply[4] != byte4 || (byte5.has_value() && reply[5] != *byte5))
    {
        events.log(LogLevel::Error, "Wrong response from ECU");
        return fail(ErrorKind::BadResponse, "Wrong response from ECU");
    }
    return {};
}

// connect_bootloader():161-318. Returns the ECU ID hex (legacy :190-203).
Result<std::string> connect_bootloader(IKlineFlashTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation, IEventSink& events,
                                       const SubaruDensoSh705xKlinePlan& plan)
{
    events.log(LogLevel::Info, "No response from kernel, initialising ECU...");
    if (Status baud = change_baud(transport, cancellation, kSsmBaud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    if (Status slept = sleep_for(clock, cancellation, kBaudSettle); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }

    events.log(LogLevel::Info, "Requesting ECU ID");
    Result<bytes::Bytes> bf = ssm_exchange(transport, clock, cancellation, plan, bytes::Bytes{0xBF}, kReadTimeout,
                                           "ECU ID request");
    if (!bf.has_value())
    {
        return std::unexpected(bf.error());
    }
    // Correction: legacy removed 8 bytes and kept 5 after checking only `> 4`.
    if (Status ok = require_ssm(*bf, 13, 0xFF, std::nullopt, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    std::string ecu_id = ecu_id_hex(bytes::ByteView(*bf).subspan(8, 5));
    events.log(LogLevel::Info, "ECU ID: " + ecu_id);

    events.log(LogLevel::Info, "Requesting to start communication");
    Result<bytes::Bytes> start = ssm_exchange(transport, clock, cancellation, plan, bytes::Bytes{0x81}, kReadTimeout,
                                              "start communication");
    if (!start.has_value())
    {
        return std::unexpected(start.error());
    }
    if (Status ok = require_ssm(*start, 5, 0xC1, std::nullopt, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Start communication ok");

    events.log(LogLevel::Info, "Requesting timings params");
    Result<bytes::Bytes> timings = ssm_exchange(transport, clock, cancellation, plan, bytes::Bytes{0x83, 0x00},
                                                kReadTimeout, "timing parameters");
    if (!timings.has_value())
    {
        return std::unexpected(timings.error());
    }
    if (Status ok = require_ssm(*timings, 5, 0xC3, std::nullopt, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Timing parameters ok");

    events.log(LogLevel::Info, "Requesting seed");
    Result<bytes::Bytes> seed_reply = ssm_exchange(transport, clock, cancellation, plan, bytes::Bytes{0x27, 0x01},
                                                   kReadTimeout, "seed request");
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    if (Status ok = require_ssm(*seed_reply, 10, 0x67, 0x01, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Seed request ok");
    const bytes::ByteView seed = bytes::ByteView(*seed_reply).subspan(6, 4);
    const bytes::Bytes key = plan.seed_key == SubaruDensoSh705xKlineSeedKey::EcuTek
                                 ? denso_sh705x_kline_ecutek_seed_key(seed)
                                 : denso_sh705x_kline_stock_seed_key(seed);

    Result<bytes::Bytes> key_reply = ssm_exchange(transport, clock, cancellation, plan,
                                                  composeBe(0x27_b, 0x02_b, key), kReadTimeout, "seed key");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    if (Status ok = require_ssm(*key_reply, 6, 0x67, 0x02, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Seed key ok");

    events.log(LogLevel::Info, "Set session mode");
    Result<bytes::Bytes> session = ssm_exchange(transport, clock, cancellation, plan,
                                                bytes::Bytes{0x10, 0x85, 0x02}, kReadTimeout, "session mode");
    if (!session.has_value())
    {
        return std::unexpected(session.error());
    }
    if (Status ok = require_ssm(*session, 5, 0x50, std::nullopt, events); !ok.has_value())
    {
        return std::unexpected(ok.error());
    }
    events.log(LogLevel::Info, "Succesfully set to programming session");
    return ecu_id;
}

// send_sid_36_transferdata(): 0x80-byte chunks, the last carrying the rest.
Status transfer_kernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                       const SubaruDensoSh705xKlinePlan& plan, std::uint32_t address, bytes::ByteView encrypted)
{
    const std::uint32_t length = static_cast<std::uint32_t>(encrypted.size()) & ~std::uint32_t{3};
    for (std::uint32_t offset = 0; offset < length; offset += kUploadChunkBytes)
    {
        const std::uint32_t chunk = std::min(kUploadChunkBytes, length - offset);
        Result<bytes::Bytes> reply =
            ssm_exchange(transport, clock, cancellation, plan,
                         composeBe(0x36_b, u24(address + offset), encrypted.subspan(offset, chunk)), kReadTimeout,
                         "kernel transfer");
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        if (reply->size() < 5 || (*reply)[4] != 0x76)
        {
            return fail(ErrorKind::BadResponse, "kernel transfer block rejected");
        }
    }
    return {};
}

// upload_kernel():331-500.
Status upload_kernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                     IEventSink& events, const SubaruDensoSh705xKlinePlan& plan, const KernelImage& kernel)
{
    if (Status baud = change_baud(transport, cancellation, kUploadBaud); !baud.has_value())
    {
        return baud;
    }
    const bytes::Bytes balanced = denso_sh705x_kline_balanced_kernel(kernel.bytes);
    const bytes::Bytes encrypted =
        denso_sh705x_kline_encrypt_payload(balanced, static_cast<std::uint32_t>(balanced.size()));
    const auto size = static_cast<std::uint32_t>(encrypted.size());

    Result<bytes::Bytes> request = ssm_exchange(transport, clock, cancellation, plan,
                                                composeBe(0x34_b, u24(kernel.load_address), 0x04_b, u24(size)),
                                                kExtraLongTimeout, "upload request");
    if (!request.has_value())
    {
        return std::unexpected(request.error());
    }
    if (Status ok = require_ssm(*request, 5, 0x74, std::nullopt, events); !ok.has_value())
    {
        return ok;
    }
    if (Status sent = transfer_kernel(transport, clock, cancellation, plan, kernel.load_address, encrypted);
        !sent.has_value())
    {
        return sent;
    }
    Result<bytes::Bytes> routine = ssm_exchange(transport, clock, cancellation, plan, bytes::Bytes{0x31, 0x01, 0x01},
                                                kExtraLongTimeout, "start routine");
    if (!routine.has_value())
    {
        return std::unexpected(routine.error());
    }
    if (Status ok = require_ssm(*routine, 5, 0x71, std::nullopt, events); !ok.has_value())
    {
        return ok;
    }
    if (Status slept = sleep_for(clock, cancellation, kPostStartRoutine); !slept.has_value())
    {
        return slept;
    }
    // Correction: legacy ignored this change_port_speed("62500") result.
    if (Status baud = change_baud(transport, cancellation, kProbeBaud); !baud.has_value())
    {
        return baud;
    }
    Result<OptionalBytes> id = request_kernel_id(transport, clock, cancellation);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (!id->has_value())
    {
        return fail(ErrorKind::Timeout, "no kernel ID after upload");
    }
    if (!kernel_reply_ok(**id, kOpId, 5))
    {
        events.log(LogLevel::Error, "Wrong response from ECU");
        return fail(ErrorKind::BadResponse, "kernel did not start after upload");
    }
    events.log(LogLevel::Info, "Kernel ID: " + std::string((**id).begin() + 5, (**id).end() - 1));
    return {};
}

// execute():79-87. Returns the ECU ID when the handshake ran.
Result<std::optional<std::string>> start_session(IKlineFlashTransport& transport, IClock& clock,
                                                 const ICancellationToken& cancellation, IEventSink& events,
                                                 const FlashPlan& plan)
{
    const auto& family = std::get<SubaruDensoSh705xKlinePlan>(plan.family_plan());
    events.log(LogLevel::Info, "Connecting to Subaru 04 32-bit K-Line bootloader, please wait...");
    Result<bool> alive = probe_kernel(transport, clock, cancellation, events);
    if (!alive.has_value())
    {
        return std::unexpected(alive.error());
    }
    if (*alive)
    {
        return std::optional<std::string>{};
    }
    Result<std::string> ecu_id = connect_bootloader(transport, clock, cancellation, events, family);
    if (!ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
    }
    events.log(LogLevel::Info, "Initializing Subaru 04 32-bit K-Line kernel upload, please wait...");
    if (Status uploaded = upload_kernel(transport, clock, cancellation, events, family, *plan.kernel());
        !uploaded.has_value())
    {
        return std::unexpected(uploaded.error());
    }
    return std::optional<std::string>{std::move(*ecu_id)};
}
```

Add `#include <algorithm>` and `#include <optional>`. Replace `execute()`'s body tail:

```cpp
Result<FlashExecutionResult> SubaruDensoSh705xKlineExecutor::execute(const FlashPlan& plan,
                                                                     IKlineFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::SubaruDensoSh705xKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    Result<std::optional<std::string>> ecu_id = start_session(transport, clock, cancellation, events, plan);
    if (!ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
    }
    return fail(ErrorKind::Unsupported, "Denso SH705x K-Line operation tail lands in tasks 7-8");
}
```

- [ ] **Step 4: Run to verify they pass**

Revert `BoundAttemptResetsBeforeConfigure`'s expectation to `IsErr(ErrorKind::Disconnected)` (the first `setBaud` now fails). Run:
`bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_kline_executor_test`
Expected: PASS. If `CancellationBeforeAnySessionWriteSendsNothingFurther` finds a boundary that sends another frame or returns another kind, fix the executor, not the test.

- [ ] **Step 5: Mutation checks**

One at a time, each restored with `git diff --exit-code` on the `.cpp`:
1. `require_ssm(*bf, 13, ...)` → `5`: expect `ShortEcuIdReplyIsRejectedBeforeSlicing` to FAIL (or crash under ASan) — record which.
2. Delete the `change_baud(... kProbeBaud)` status check after `31` (ignore its result): expect `FinalBaudChangeFailureIsNowChecked` to FAIL.
3. Swap the ECUTEK/stock branch: expect `EcutekProtocolSendsTheEcutekKey` to FAIL.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.cpp src/backend/flash/ecu/subaru_denso_sh705x_kline_executor_test.cpp
git commit -m "feat(flash): port the Denso SH705x K-Line bootloader session"
```

### Task 7: Read

**Files:**
- Modify: `src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.cpp`
- Test: `src/backend/flash/ecu/subaru_denso_sh705x_kline_executor_test.cpp`

**Interfaces:**
- Consumes: Task 6's `start_session()`.
- Produces (anonymous namespace): `Result<bytes::Bytes> read_mem(IKlineFlashTransport&, IClock&, const ICancellationToken&, IEventSink&, const MemoryRegion& region)`. `execute()` returns `FlashExecutionResult{.operation = Read, .read_bytes = rom, .rom_id = ecu_id + "_"}` (no `rom_id` when the kernel was already alive).

- [ ] **Step 1: Write the failing tests**

```cpp
// read_mem(): READ_AREA [00, addr24, 0x0400], reply BE EF len 43 <0x400 bytes> sum8.
bytes::Bytes read_request(std::uint32_t address)
{
    return beef(0x03, composeBe(0x00_b, u24(address), std::uint16_t{0x0400}));
}
bytes::Bytes page_reply(std::uint32_t address)
{
    bytes::Bytes data(0x400);
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<bytes::Byte>((address >> 10) + i);
    }
    return beef_reply(0x03, data);
}

TEST(SubaruDensoSh705xKlineExecutor, ReadReturnsTheWholeRomAndTheRomId)
{
    Harness h;
    script_session(h.transport);
    const std::uint32_t romsize = find_flash_device("SH7055")->romsize;
    bytes::Bytes expected;
    for (std::uint32_t address = 0; address < romsize; address += 0x400)
    {
        h.transport.exchange(read_request(address), page_reply(address));
        const bytes::Bytes reply = page_reply(address);
        expected.insert(expected.end(), reply.begin() + 5, reply.end() - 1);
    }

    const auto result = h.run(make_plan(FlashOperation::Read));

    ASSERT_THAT(result, IsOk());
    EXPECT_TRUE(h.transport.scriptConsumed());
    EXPECT_EQ(result->read_bytes, expected);
    // execute()/connect_bootloader():206 RomId = ecuid + "_".
    EXPECT_EQ(result->rom_id, std::optional<std::string>("4142434445_"));
}

TEST(SubaruDensoSh705xKlineExecutor, ReadWithLiveKernelHasNoRomId)
{
    Harness h;
    script_probe_alive(h.transport);
    const std::uint32_t romsize = find_flash_device("SH7055")->romsize;
    for (std::uint32_t address = 0; address < romsize; address += 0x400)
    {
        h.transport.exchange(read_request(address), page_reply(address));
    }
    const auto result = h.run(make_plan(FlashOperation::Read));
    ASSERT_THAT(result, IsOk());
    EXPECT_FALSE(result->rom_id.has_value());
}

TEST(SubaruDensoSh705xKlineExecutor, ReadRejectsShortBadChecksumAndWrongOpcodePages)
{
    bytes::Bytes short_page = page_reply(0);
    short_page.erase(short_page.begin() + 10); // one data byte missing
    bytes::Bytes bad_sum = page_reply(0);
    bad_sum.back() ^= 0x01;
    bytes::Bytes wrong_op = page_reply(0);
    wrong_op[4] = 0x7F;
    for (const bytes::Bytes& reply : {short_page, bad_sum, wrong_op})
    {
        Harness h;
        script_session(h.transport);
        h.transport.exchange(read_request(0), reply);
        // Correction: legacy appended any reply with size > 5.
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed()); // no second page requested
    }
}

TEST(SubaruDensoSh705xKlineExecutor, ReadPropagatesTransportErrorsAndCancellation)
{
    {
        Harness h;
        script_session(h.transport);
        h.transport.expectWrite(read_request(0));
        h.transport.queue_error(ErrorKind::Disconnected, "unplugged");
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Disconnected));
    }
    {
        Harness h;
        script_session(h.transport);
        h.transport.expectWrite(read_request(0));
        h.transport.queue_no_frame();
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Timeout));
    }
    {
        Harness h;
        script_session(h.transport);
        h.transport.exchange(read_request(0), page_reply(0));
        h.transport.exchange(read_request(0x400), page_reply(0x400));
        // Cancel once the second page request is out: its reply is never read
        // and no third page is requested.
        h.cancellation.set_predicate([&h] { return h.transport.writesConsumed() >= kSessionWrites + 2; });
        EXPECT_THAT(h.run(make_plan(FlashOperation::Read)), IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(h.transport.writesConsumed(), kSessionWrites + 2);
    }
}
```

- [ ] **Step 2: Run to verify they fail**

Run the executor test. Expected: new tests FAIL with `Unsupported`.

- [ ] **Step 3: Implement read**

```cpp
// read_mem():503-640. 24-bit page address, 0x400-byte pages, 3000ms, no settle.
Result<bytes::Bytes> read_mem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const MemoryRegion& region)
{
    bytes::Bytes rom;
    rom.reserve(region.length);
    events.progress(0, static_cast<int>(region.length));
    for (std::uint32_t address = region.start; address < region.start + region.length; address += kReadPageSize)
    {
        const bytes::Bytes request = frame(kOpReadArea, composeBe(0x00_b, u24(address), std::uint16_t(kReadPageSize)));
        Result<bytes::Bytes> reply = required_exchange(transport, clock, cancellation, request, 0ms,
                                                       kExtraLongTimeout, "read");
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        // Correction: require the complete page and its sum8 before accepting it.
        const bytes::Bytes& page = *reply;
        if (page.size() != kReadPageSize + 6 || !kernel_reply_ok(page, kOpReadArea, kReadPageSize + 6) ||
            page.back() != bytes::sum8(bytes::ByteView(page).first(page.size() - 1)))
        {
            events.log(LogLevel::Error, "Wrong response from ECU");
            return fail(ErrorKind::BadResponse, std::format("incomplete or corrupt page at 0x{:06X}", address));
        }
        rom.insert(rom.end(), page.begin() + 5, page.end() - 1);
        events.progress(static_cast<int>(rom.size()), static_cast<int>(region.length));
    }
    rom.resize(region.length);
    return rom;
}
```

In `execute()`, replace the `Unsupported` tail:

```cpp
    if (plan.operation() == FlashOperation::Read)
    {
        events.log(LogLevel::Info, "Reading ROM from Subaru 04 32-bit using K-Line");
        Result<bytes::Bytes> rom = read_mem(transport, clock, cancellation, events, plan.transfer_region());
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        std::optional<std::string> rom_id;
        if (ecu_id->has_value())
        {
            rom_id = **ecu_id + "_"; // connect_bootloader(): RomId = ecuid + "_"
        }
        return FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::move(*rom), .rom_id = rom_id};
    }
    return fail(ErrorKind::Unsupported, "Denso SH705x K-Line write lands in task 8");
```

- [ ] **Step 4: Run to verify they pass**

Run the executor test. Expected: PASS.

- [ ] **Step 5: Mutation check**

Remove the `page.back() != bytes::sum8(...)` clause. Expect `ReadRejectsShortBadChecksumAndWrongOpcodePages` to FAIL. Restore.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.cpp src/backend/flash/ecu/subaru_denso_sh705x_kline_executor_test.cpp
git commit -m "feat(flash): port the Denso SH705x K-Line ROM read"
```

### Task 8: Write and TestWrite

**Files:**
- Modify: `src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.cpp`
- Test: `src/backend/flash/ecu/subaru_denso_sh705x_kline_executor_test.cpp`

**Interfaces:**
- Produces (anonymous namespace): `Result<std::uint32_t> read_block_crc(...)`, `Result<std::vector<bool>> compare_blocks(...)`, `Status init_flash_write(..., bool test_write)`, `Status flash_block(..., bytes::ByteView image, const MemoryRegion& block, bool test_write, std::uint64_t& written, std::uint64_t total)`, `Status write_mem(...)`. `execute()` returns `FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::nullopt}` on success.

- [ ] **Step 1: Write the failing tests**

```cpp
// check_romcrc(): CRC [addr32, 00, len24] -> BE EF len 42 crc32 ... ; then read(200) flush.
bytes::Bytes crc_request(const flashblock& block)
{
    return beef(0x02, composeBe(block.start, 0x00_b, u24(block.len)));
}
void script_compare(ScriptedKlineFlashTransport& t, std::string_view mcu, const bytes::Bytes& image,
                    const std::vector<unsigned>& differing)
{
    auto s = t.section("compare");
    const flashdev_t *device = find_flash_device(mcu);
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        const flashblock& block = device->fblocks[i];
        std::uint32_t crc = fastecu::checksum::crc32(bytes::ByteView(image).subspan(block.start, block.len));
        if (std::ranges::find(differing, i) != differing.end())
        {
            crc ^= 0xFFFFFFFFU;
        }
        t.exchange(crc_request(block), beef_reply(0x02, composeBe(crc)));
        t.queue_no_frame(); // the 200ms flush read after every compare
    }
}
void script_init(ScriptedKlineFlashTransport& t, std::uint8_t mode_opcode)
{
    auto s = t.section("init_flash_write");
    t.exchange(beef(0x05), beef_reply(0x05, composeBe(std::uint32_t{0x00000204})));
    t.exchange(beef(0x06), beef_reply(0x06, composeBe(std::uint32_t{0x00001000})));
    t.exchange(beef(mode_opcode), beef_reply(mode_opcode, bytes::Bytes{0x00}));
}
void script_reflash(ScriptedKlineFlashTransport& t, const bytes::Bytes& image, const flashblock& block,
                    std::uint8_t commit_opcode)
{
    auto s = t.section("reflash_block");
    t.exchange(beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0x00, 0x00, 0x00})); // 900/50 = 18.0V
    t.exchange(beef(0x25, composeBe(block.start)), beef_reply(0x25));
    for (std::uint32_t address = block.start; address < block.start + block.len; address += 0x200)
    {
        t.exchange(beef(0x22, composeBe(address, bytes::ByteView(image).subspan(address, 0x200))),
                   beef_reply(0x22, bytes::Bytes{0x00}));
        if ((address + 0x200 - block.start) % 0x1000 == 0)
        {
            const std::uint32_t commit_start = address + 0x200 - 0x1000;
            const std::uint32_t crc = fastecu::checksum::crc32(bytes::ByteView(image).subspan(commit_start, 0x1000));
            t.exchange(beef(commit_opcode, composeBe(commit_start, std::uint16_t{0x1000}, crc)),
                       beef_reply(commit_opcode, bytes::Bytes{0x00}));
        }
    }
}

bytes::Bytes sh7055_image()
{
    bytes::Bytes image(find_flash_device("SH7055")->romsize);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i * 7);
    }
    return image;
}

TEST(SubaruDensoSh705xKlineExecutor, WriteWithNoDifferencesFlashesNothing)
{
    Harness h;
    const bytes::Bytes image = sh7055_image();
    script_session(h.transport);
    script_compare(h.transport, "SH7055", image, {});
    EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)), IsOk());
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, WriteReflashesOnlyChangedBlocksWithCommit)
{
    Harness h;
    const bytes::Bytes image = sh7055_image();
    const flashdev_t *device = find_flash_device("SH7055");
    script_session(h.transport);
    script_compare(h.transport, "SH7055", image, {1, 8});
    script_init(h.transport, 0x20);
    script_reflash(h.transport, image, device->fblocks[1], 0x24);
    script_reflash(h.transport, image, device->fblocks[8], 0x24);
    script_compare(h.transport, "SH7055", image, {});

    EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)), IsOk());
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, TestWriteUsesFlashDisableAndValidateNeverEnableOrCommit)
{
    Harness h;
    const bytes::Bytes image = sh7055_image();
    const flashdev_t *device = find_flash_device("SH7055");
    script_session(h.transport);
    script_compare(h.transport, "SH7055", image, {0});
    script_init(h.transport, 0x21);
    script_reflash(h.transport, image, device->fblocks[0], 0x23);
    script_compare(h.transport, "SH7055", image, {0}); // nothing committed, still differs

    EXPECT_THAT(h.run(make_plan(FlashOperation::TestWrite, "sub_ecu_denso_sh7055_04_cobb", "SH7055", image)),
                IsOk());
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, BadFlashModeAckSendsNoEraseInEitherMode)
{
    for (const auto& [operation, opcode] :
         {std::pair{FlashOperation::TestWrite, std::uint8_t{0x21}}, std::pair{FlashOperation::Write, std::uint8_t{0x20}}})
    {
        SCOPED_TRACE(static_cast<int>(operation));
        Harness h;
        const bytes::Bytes image = sh7055_image();
        script_session(h.transport);
        script_compare(h.transport, "SH7055", image, {0});
        h.transport.exchange(beef(0x05), beef_reply(0x05, composeBe(std::uint32_t{0x204})));
        h.transport.exchange(beef(0x06), beef_reply(0x06, composeBe(std::uint32_t{0x1000})));
        h.transport.exchange(beef(opcode), bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, opcode, 0x00});

        EXPECT_THAT(h.run(make_plan(operation, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed()); // no PROG_VOLT, no BLANK_PAGE
    }
}

TEST(SubaruDensoSh705xKlineExecutor, WriteThatStillDiffersAfterReflashFails)
{
    // Correction: legacy logged "ERROR IN FLASH PROCESS" and returned success.
    Harness h;
    const bytes::Bytes image = sh7055_image();
    const flashdev_t *device = find_flash_device("SH7055");
    script_session(h.transport);
    script_compare(h.transport, "SH7055", image, {2});
    script_init(h.transport, 0x20);
    script_reflash(h.transport, image, device->fblocks[2], 0x24);
    script_compare(h.transport, "SH7055", image, {2});

    EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                IsErr(ErrorKind::BadResponse));
}

TEST(SubaruDensoSh705xKlineExecutor, ShortCrcReplyIsRejectedBeforeParsing)
{
    // Correction: legacy read at(5..8) after checking only `> 5`.
    Harness h;
    const bytes::Bytes image = sh7055_image();
    script_session(h.transport);
    h.transport.exchange(crc_request(find_flash_device("SH7055")->fblocks[0]),
                         bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x42, 0x12, 0x00});
    EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(h.transport.scriptConsumed());
}

TEST(SubaruDensoSh705xKlineExecutor, EachRejectedWriteStepStopsLaterCommands)
{
    // Reject PROG_VOLT, BLANK_PAGE, the first WRITE_FLASH_BUFFER, then COMMIT.
    const flashdev_t *device = find_flash_device("SH7055");
    const flashblock block = device->fblocks[0];
    const bytes::Bytes image = sh7055_image();
    const std::uint32_t crc0 = fastecu::checksum::crc32(bytes::ByteView(image).subspan(0, 0x1000));
    const std::vector<std::vector<std::pair<bytes::Bytes, bytes::Bytes>>> prefixes{
        {{beef(0x04), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00}}},
        {{beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0})},
         {beef(0x25, composeBe(block.start)), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x00}}},
        {{beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0})},
         {beef(0x25, composeBe(block.start)), beef_reply(0x25)},
         {beef(0x22, composeBe(std::uint32_t{0}, bytes::ByteView(image).subspan(0, 0x200))),
          bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, 0x22, 0x00}}},
    };
    for (const auto& prefix : prefixes)
    {
        Harness h;
        script_session(h.transport);
        script_compare(h.transport, "SH7055", image, {0});
        script_init(h.transport, 0x20);
        for (const auto& [request, reply] : prefix)
        {
            h.transport.exchange(request, reply);
        }
        EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed());
    }
    {
        Harness h;
        script_session(h.transport);
        script_compare(h.transport, "SH7055", image, {0});
        script_init(h.transport, 0x20);
        h.transport.exchange(beef(0x04), beef_reply(0x04, bytes::Bytes{0x03, 0x84, 0, 0, 0}));
        h.transport.exchange(beef(0x25, composeBe(block.start)), beef_reply(0x25));
        for (std::uint32_t address = 0; address < 0x1000; address += 0x200)
        {
            h.transport.exchange(beef(0x22, composeBe(address, bytes::ByteView(image).subspan(address, 0x200))),
                                 beef_reply(0x22, bytes::Bytes{0x00}));
        }
        h.transport.exchange(beef(0x24, composeBe(std::uint32_t{0}, std::uint16_t{0x1000}, crc0)),
                             bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x7F, 0x24, 0x00});
        EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::BadResponse));
        EXPECT_TRUE(h.transport.scriptConsumed());
    }
}

TEST(SubaruDensoSh705xKlineExecutor, CompareUsesLegacyTimeoutsAndPacing)
{
    struct RecordingClock final : FakeClock
    {
        Status sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
        {
            sleeps.push_back(duration);
            return FakeClock::sleep(duration, cancellation);
        }
        std::vector<std::chrono::milliseconds> sleeps;
    };
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    const bytes::Bytes image = sh7055_image();
    script_probe_alive(transport);
    script_compare(transport, "SH7055", image, {});
    RecordingClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    ASSERT_THAT(executor.execute(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image),
                                 transport, clock, cancellation, events),
                IsOk());
    // probe: 100 settle + 200 kernel-ID settle; then 16 x 5ms block pacing.
    std::vector<std::chrono::milliseconds> expected_sleeps{100ms, 200ms};
    expected_sleeps.insert(expected_sleeps.end(), 16, 5ms);
    EXPECT_EQ(clock.sleeps, expected_sleeps);
    std::vector<std::chrono::milliseconds> expected_reads{800ms};
    for (int i = 0; i < 16; ++i)
    {
        expected_reads.push_back(3000ms);
        expected_reads.push_back(200ms);
    }
    EXPECT_EQ(transport.read_timeouts_, expected_reads);
}

TEST(SubaruDensoSh705xKlineExecutor, CancellationDuringWriteStopsBeforeTheNextCommand)
{
    // Script: probe 1 + compare 16 + init 3 + PROG_VOLT 1 + BLANK_PAGE 1 +
    // 8 chunks + 1 commit + compare 16 = 47 writes. Cancelling once k are out
    // must send no further frame.
    for (std::size_t k = 1; k < 47; k += 3)
    {
        SCOPED_TRACE(k);
        Harness h;
        const bytes::Bytes image = sh7055_image();
        script_probe_alive(h.transport);
        script_compare(h.transport, "SH7055", image, {0});
        script_init(h.transport, 0x20);
        script_reflash(h.transport, image, find_flash_device("SH7055")->fblocks[0], 0x24);
        script_compare(h.transport, "SH7055", image, {});
        h.cancellation.set_predicate([&h, k] { return h.transport.writesConsumed() >= k; });
        EXPECT_THAT(h.run(make_plan(FlashOperation::Write, "sub_ecu_denso_sh7055_04", "SH7055", image)),
                    IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(h.transport.writesConsumed(), k);
    }
}
```

Add `#include <algorithm>` for `std::ranges::find`. Confirm `flashblock`'s field names are `start` and `len` in `kernelmemorymodels.h`.

- [ ] **Step 2: Run to verify they fail**

Expected: the new tests FAIL with `Unsupported`.

- [ ] **Step 3: Implement write**

```cpp
// check_romcrc():799-882: CRC request, 3000ms, then a 200ms flush read after
// either outcome.
Result<std::uint32_t> read_block_crc(IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events,
                                     const MemoryRegion& block)
{
    const bytes::Bytes request = frame(kOpCrc, composeBe(block.start, 0x00_b, u24(block.length)));
    Result<bytes::Bytes> reply =
        required_exchange(transport, clock, cancellation, request, 0ms, kExtraLongTimeout, "CRC check");
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    // Correction: bytes 5..8 are read, so require at least nine bytes.
    if (!kernel_reply_ok(*reply, kOpCrc, 9))
    {
        events.log(LogLevel::Error, "Wrong response from ECU");
        return fail(ErrorKind::BadResponse, "Wrong response from ECU during CRC check");
    }
    const std::uint32_t crc = bytes::readU32Be(*reply, 5);
    if (Status cancelled = check_cancelled(cancellation, "cancelled before CRC flush"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (Result<OptionalBytes> flushed = transport.read(kShortTimeout, cancellation); !flushed.has_value())
    {
        return std::unexpected(flushed.error());
    }
    return crc;
}

// get_changed_blocks():762-797.
Result<std::vector<bool>> compare_blocks(IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events,
                                         const flashdev_t& device, bytes::ByteView image)
{
    events.log(LogLevel::Info, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    std::vector<bool> changed(device.numblocks, false);
    for (unsigned i = 0; i < device.numblocks; ++i)
    {
        const MemoryRegion block{device.fblocks[i].start, device.fblocks[i].len};
        Result<std::uint32_t> ecu_crc = read_block_crc(transport, clock, cancellation, events, block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::uint32_t image_crc = fastecu::checksum::crc32(image.subspan(block.start, block.length));
        changed[i] = *ecu_crc != image_crc;
        events.log(LogLevel::Info, std::format("FB{:02}\t0x{:08X}\t0x{:08X}\t{:08X}\t{:08X}\t{}", i, block.start,
                                               block.length, *ecu_crc, image_crc, changed[i] ? "NO" : "YES"));
        if (Status slept = sleep_for(clock, cancellation, kCrcBlockPacing); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    return changed;
}

Status require_kernel_reply(const Result<bytes::Bytes>& reply, std::uint8_t opcode, std::size_t min_size,
                            IEventSink& events)
{
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!kernel_reply_ok(*reply, opcode, min_size))
    {
        events.log(LogLevel::Error, "Wrong response from ECU");
        return fail(ErrorKind::BadResponse, std::format("Wrong response from ECU to kernel opcode 0x{:02X}", opcode));
    }
    return {};
}

// init_flash_write():884-1030. Sizes are queried and logged only; legacy then
// hard-codes 0x200/0x1000 in flash_block(). The mode ack gates everything after.
Status init_flash_write(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                        IEventSink& events, bool test_write)
{
    for (const std::uint8_t opcode : {kOpGetMaxMsgSize, kOpGetMaxBlockSize})
    {
        Result<bytes::Bytes> reply =
            required_exchange(transport, clock, cancellation, frame(opcode), 0ms, kMediumTimeout, "flash init");
        if (Status ok = require_kernel_reply(reply, opcode, 10, events); !ok.has_value())
        {
            return ok;
        }
        events.log(LogLevel::Info, std::format("{}: 0x{:04x}",
                                               opcode == kOpGetMaxMsgSize ? "Check max message length"
                                                                          : "Check flashblock size",
                                               bytes::readU32Be(*reply, 5)));
    }
    const std::uint8_t mode = test_write ? kOpFlashDisable : kOpFlashEnable;
    events.log(LogLevel::Info, test_write ? "Test write mode on, no actual flash write is performed"
                                          : "Test write mode off, perform actual flash write");
    Result<bytes::Bytes> reply =
        required_exchange(transport, clock, cancellation, frame(mode), 0ms, kMediumTimeout, "flash mode");
    if (Status ok = require_kernel_reply(reply, mode, 6, events); !ok.has_value())
    {
        return ok;
    }
    events.log(LogLevel::Info, "Flash mode succesfully set");
    return {};
}

// reflash_block():1034-1123 and flash_block():1125-1366.
Status flash_block(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events, bytes::ByteView image, const MemoryRegion& block, bool test_write,
                   std::uint64_t& written, std::uint64_t total)
{
    Result<bytes::Bytes> volt =
        required_exchange(transport, clock, cancellation, frame(kOpProgVolt), 0ms, kMediumTimeout, "prog voltage");
    if (Status ok = require_kernel_reply(volt, kOpProgVolt, 10, events); !ok.has_value())
    {
        return ok;
    }
    events.log(LogLevel::Info,
               std::format("Programming voltage: {:.2f}V", bytes::readU16Be(*volt, 5) / 50.0));

    Result<bytes::Bytes> erased = required_exchange(transport, clock, cancellation,
                                                    frame(kOpBlankPage, composeBe(block.start)), 0ms,
                                                    kExtraLongTimeout, "erase");
    if (Status ok = require_kernel_reply(erased, kOpBlankPage, 5, events); !ok.has_value())
    {
        return ok;
    }

    std::uint32_t commit_start = block.start;
    for (std::uint32_t address = block.start; address < block.start + block.length; address += kWriteChunkSize)
    {
        Result<bytes::Bytes> chunk = required_exchange(
            transport, clock, cancellation,
            frame(kOpWriteFlashBuffer, composeBe(address, image.subspan(address, kWriteChunkSize))), 0ms,
            kExtraLongTimeout, "write");
        if (Status ok = require_kernel_reply(chunk, kOpWriteFlashBuffer, 6, events); !ok.has_value())
        {
            return ok;
        }
        written += kWriteChunkSize;
        events.progress(static_cast<int>(written), static_cast<int>(total));
        if (commit_start + kCommitBlockSize == address + kWriteChunkSize)
        {
            const std::uint8_t commit = test_write ? kOpValidateFlashBuffer : kOpCommitFlashBuffer;
            const std::uint32_t crc = fastecu::checksum::crc32(image.subspan(commit_start, kCommitBlockSize));
            Result<bytes::Bytes> committed = required_exchange(
                transport, clock, cancellation,
                frame(commit, composeBe(commit_start, std::uint16_t(kCommitBlockSize), crc)), 0ms,
                kExtraLongTimeout, "commit");
            if (Status ok = require_kernel_reply(committed, commit, 6, events); !ok.has_value())
            {
                return ok;
            }
            commit_start += kCommitBlockSize;
        }
    }
    return {};
}

// write_mem():641-760.
Status write_mem(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                 IEventSink& events, const FlashPlan& plan)
{
    const flashdev_t *device = find_flash_device(plan.mcu_name());
    const bytes::ByteView image = *plan.image();
    const bool test_write = plan.operation() == FlashOperation::TestWrite;

    events.log(LogLevel::Info, "--- Comparing ECU flash memory pages to image file ---");
    Result<std::vector<bool>> changed = compare_blocks(transport, clock, cancellation, events, *device, image);
    if (!changed.has_value())
    {
        return std::unexpected(changed.error());
    }
    std::uint64_t total = 0;
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        if ((*changed)[i])
        {
            total += device->fblocks[i].len;
        }
    }
    if (total == 0)
    {
        events.log(LogLevel::Info,
                   "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
        return {};
    }

    events.log(LogLevel::Info, "--- Start writing ROM file to ECU flash memory ---");
    if (Status ok = init_flash_write(transport, clock, cancellation, events, test_write); !ok.has_value())
    {
        return ok;
    }
    std::uint64_t written = 0;
    events.progress(0, static_cast<int>(total));
    for (unsigned i = 0; i < device->numblocks; ++i)
    {
        if (!(*changed)[i])
        {
            continue;
        }
        const MemoryRegion block{device->fblocks[i].start, device->fblocks[i].len};
        if (Status ok = flash_block(transport, clock, cancellation, events, image, block, test_write, written, total);
            !ok.has_value())
        {
            events.log(LogLevel::Info, std::format("Block {} reflash failed.", i));
            return ok;
        }
        events.log(LogLevel::Info, std::format("Block {} reflash complete.", i));
    }

    events.log(LogLevel::Info, "--- Comparing ECU flash memory pages to image file after reflash ---");
    Result<std::vector<bool>> after = compare_blocks(transport, clock, cancellation, events, *device, image);
    if (!after.has_value())
    {
        return std::unexpected(after.error());
    }
    if (test_write)
    {
        events.log(LogLevel::Info, "*** Test write PASS, it's ok to perform actual write! ***");
        return {};
    }
    if (std::ranges::find(*after, true) != after->end())
    {
        events.log(LogLevel::Error, "*** ERROR IN FLASH PROCESS ***");
        events.log(LogLevel::Error,
                   "Don't power off your ECU, kernel is still running and you can try flashing again!");
        // Correction: legacy returned success here.
        return fail(ErrorKind::BadResponse, "flash verification still differs after reflash");
    }
    return {};
}
```

Add `#include <vector>`. In `execute()`, replace the Task 7 `Unsupported` tail:

```cpp
    events.log(LogLevel::Info, "Writing ROM to Subaru 04 32-bit using K-Line");
    if (Status written = write_mem(transport, clock, cancellation, events, plan); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::nullopt};
```

Legacy log strings in the snippets above were taken from the cited functions; compare each against the legacy file before it is deleted and correct any drift.

- [ ] **Step 4: Run to verify they pass**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_kline_executor_test`
Expected: PASS.

- [ ] **Step 5: Mutation checks**

Each restored with `git diff --exit-code`:
1. Swap `kOpFlashDisable`/`kOpFlashEnable` in `init_flash_write`: expect `TestWriteUsesFlashDisableAndValidateNeverEnableOrCommit` to FAIL.
2. Swap `kOpValidateFlashBuffer`/`kOpCommitFlashBuffer`: same test FAILS.
3. Make `init_flash_write` ignore the mode ack (`return {};` after the exchange): expect `BadFlashModeAckSendsNoEraseInEitherMode` to FAIL.
4. Replace the post-compare `return fail(...)` with `return {};`: expect `WriteThatStillDiffersAfterReflashFails` to FAIL.
5. `kernel_reply_ok(*reply, kOpCrc, 9)` → `6`: expect `ShortCrcReplyIsRejectedBeforeParsing` to FAIL (or ASan), record which.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.cpp src/backend/flash/ecu/subaru_denso_sh705x_kline_executor_test.cpp
git commit -m "feat(flash): port the Denso SH705x K-Line write and test write"
```

### Task 9: Desktop workflow, dispatch cut-over, legacy deletion

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp`, `flash_workflow_test.cpp`, `src/platform/desktop/common/flash/BUILD.bazel`
- Modify: `src/ui/desktop/mainwindow.cpp:1253-1267`, `src/ui/desktop/mainwindow.h:68`, `src/ui/desktop/flash/ecu/BUILD.bazel`
- Delete: `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_kline.{h,cpp}`, `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.{h,cpp}`
- Modify: `scripts/check-legacy-flash-drain.py`

**Interfaces:**
- Consumes: `build_subaru_denso_sh705x_kline_plan`, `SubaruDensoSh705xKlineExecutor`, `resolveKernel(request, repository)`, `DesktopKlineFlashTransport`, `QtClock`, `FlashAttemptOutcome`.
- Produces: `Route::Kind::SubaruDensoSh705xKline`, six `RouteMatch::Exact` routes, `class SubaruDensoSh705xKlineWorkflow`.

- [ ] **Step 1: Write the failing workflow tests**

In `flash_workflow_test.cpp`, extend `catalogPaths()`'s XML `<protocols>` with:

```xml
    <protocol name="sub_ecu_denso_sh7055_04" alias="sti04">
      <ecu>Denso SH7055</ecu><mcu>SH7055</mcu>
      <kernel>catalog_kline_sh7055.bin</kernel><kernel_addr>0xFFFF6004</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_ecutek" alias="sti05_ecutek">
      <ecu>Denso SH7058</ecu><mcu>SH7058</mcu>
      <kernel>catalog_kline_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_cobb" alias="sti05_cobb">
      <ecu>Denso SH7058</ecu><mcu>SH7058</mcu>
      <kernel>catalog_kline_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
```

and to the kernel-file writes:

```cpp
            !writeFile(kernel_directory + "/catalog_kline_sh7055.bin", QByteArray::fromHex("aabbccdd")) ||
            !writeFile(kernel_directory + "/catalog_kline_sh7058.bin", QByteArray::fromHex("01020304")) ||
```

Declare three slots and add them:

```cpp
void FlashWorkflowTest::densoSh705xKlineRoutesExactProtocolsThroughBeginToAttempt()
{
    struct Case
    {
        const char *protocol;
        const char *mcu;
        FlashOperation operation;
        SubaruDensoSh705xKlineSeedKey seed_key;
        bytes::Bytes kernel;
    };
    const std::vector<Case> cases{
        {"sub_ecu_denso_sh7055_04", "SH7055", FlashOperation::Read, SubaruDensoSh705xKlineSeedKey::Stock,
         {0xaa, 0xbb, 0xcc, 0xdd}},
        {"sub_ecu_denso_sh7058_ecutek", "SH7058", FlashOperation::Read, SubaruDensoSh705xKlineSeedKey::EcuTek,
         {0x01, 0x02, 0x03, 0x04}},
        {"sub_ecu_denso_sh7058_cobb", "SH7058", FlashOperation::TestWrite, SubaruDensoSh705xKlineSeedKey::Stock,
         {0x01, 0x02, 0x03, 0x04}},
    };
    for (const Case& c : cases)
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto paths = catalogPaths(directory);
        QVERIFY(paths.has_value());
        auto input = request(c.protocol, c.operation);
        input.mcu = c.mcu;
        input.paths = *paths;
        if (c.operation != FlashOperation::Read)
        {
            input.image = bytes::Bytes(1024 * 1024, 0xff);
        }
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY2(workflow != nullptr, c.protocol);

        QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        auto step = workflow->next();
        QVERIFY2(std::holds_alternative<FlashAttempt>(step), c.protocol);
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        QCOMPARE(plan.family(), FlashFamily::SubaruDensoSh705xKline);
        QCOMPARE(plan.target_id(), std::string(c.protocol));
        QVERIFY(plan.kernel().has_value());
        QCOMPARE(plan.kernel()->bytes, c.kernel);
        QCOMPARE(std::get<SubaruDensoSh705xKlinePlan>(plan.family_plan()).seed_key, c.seed_key);
    }
}

void FlashWorkflowTest::densoSh705xKlineCobbReadFailsBeforeAttempt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto input = request("sub_ecu_denso_sh7058_cobb");
    input.mcu = "SH7058";
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

void FlashWorkflowTest::densoSh705xKlineIgnoresPrefixLookalikes()
{
    for (const char *near_miss : {"sub_ecu_denso_sh7055_04_future", "sub_ecu_denso_sh7058_extra",
                                  "sub_ecu_denso_sh7058_ecutek_racerom"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr, near_miss);
    }
}
```

Include `src/backend/flash/ecu/subaru_denso_sh705x_kline_types.h` in the test if `flash_types.h` does not already bring it.

- [ ] **Step 2: Run to verify they fail**

Run: `bazel test --config=release //src/platform/desktop/common/flash:flash_workflow_test` (use the target name the BUILD file gives this suite; `grep -n flash_workflow_test src/platform/desktop/common/flash/BUILD.bazel`).
Expected: FAIL — the routes return `nullptr`.

- [ ] **Step 3: Add the workflow and routes**

In `flash_workflow.cpp` add includes for `subaru_denso_sh705x_kline_executor.h` and `subaru_denso_sh705x_kline_plan.h`, then add after `SubaruDensoSh7055_02Workflow`:

```cpp
// Wave 6b-2. Resolves the kernel on the first step (a missing kernel fails
// before any prompt), then the shared Begin prompt -- the legacy dialog's only
// prompt, "Turn ignition ON" -- then the attempt. No ConfirmationSpec.
class SubaruDensoSh705xKlineWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruDensoSh705xKlineWorkflow(FlashWorkflowRequest request) : request_(std::move(request))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!plan_.has_value())
        {
            QtFileRepository repository;
            Result<KernelImage> kernel = resolveKernel(request_, repository);
            if (!kernel.has_value())
            {
                plan_ = std::unexpected(kernel.error());
            }
            else
            {
                plan_ = build_subaru_denso_sh705x_kline_plan(request_.operation, request_.protocol, request_.mcu,
                                                             std::move(request_.image), std::move(*kernel));
            }
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
            attempted_ = true;
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     bind_flash_attempt(std::move(**plan_),
                                                        std::make_unique<SubaruDensoSh705xKlineExecutor>(),
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
    std::optional<Result<FlashPlan>> plan_;
    bool begun_ = false;
    bool attempted_ = false;
    FlashAttemptOutcome outcome_;
};
```

Add `SubaruDensoSh705xKline,` to `Route::Kind` (before `Unrouted`), these rows to `kRoutes` after the `sub_ecu_denso_sh7055_02` row:

```cpp
    {"sub_ecu_denso_sh7055_04", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7055_04_ecutek", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7055_04_cobb", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_ecutek", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_cobb", SubaruDensoSh705xKline, RouteMatch::Exact},
```

and to the factory switch:

```cpp
    case SubaruDensoSh705xKline:
        return std::make_unique<SubaruDensoSh705xKlineWorkflow>(std::move(request));
```

Add `"//src/backend/flash/ecu:subaru_denso_sh705x_kline_executor"` and `"//src/backend/flash/ecu:subaru_denso_sh705x_kline_plan"` to the `flash_workflow` target's deps in `src/platform/desktop/common/flash/BUILD.bazel`.

- [ ] **Step 4: Run to verify they pass**

Run the workflow suite. Expected: PASS, including every pre-existing slot (the added catalog entries must not change earlier assertions).

- [ ] **Step 5: Cut over MainWindow and delete the legacy path**

- In `src/ui/desktop/mainwindow.cpp`, delete the `/* Denso ECU */` block's two branches that construct `FlashEcuSubaruDensoSH705xKline` (the `startsWith("sub_ecu_denso_sh7055_04")` branch and the three-name `sub_ecu_denso_sh7058` branch). The factory at line ~1200 now handles all six names before this chain runs.
- In `src/ui/desktop/mainwindow.h`, delete `#include "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_kline.h"`.
- In `src/ui/desktop/flash/ecu/BUILD.bazel`, remove `flash_ecu_subaru_denso_sh705x_kline.cpp` from `srcs` and `.h` from `hdrs`.
- Delete the four files:
  ```bash
  git rm src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_kline.h \
         src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_kline.cpp \
         src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.h \
         src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp
  ```
- In `scripts/check-legacy-flash-drain.py`, remove the line `"ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp",` from `REMAINING`.
- Confirm nothing else references the class: `grep -rn "FlashEcuSubaruDensoSH705xKline\|flash_ecu_subaru_denso_sh705x_kline" src apps tests` — expect only the flash qualification matrix/docs.

- [ ] **Step 6: Build and run the affected suites**

```bash
bazel build --config=release //:fastecu
bazel test --config=release //src/platform/desktop/common/flash:all //src/backend/flash/...
python3 scripts/check-legacy-flash-drain.py   # expect "OK: 4 families remaining, none added."
```
Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add -A src/platform/desktop/common/flash src/ui/desktop scripts/check-legacy-flash-drain.py
git commit -m "feat(flash): route Denso SH705x K-Line through the portable workflow"
```

### Task 10: Docs, gates, PR

**Files:**
- Modify: `docs/flash-qualification-matrix.md` (row `FlashEcuSubaruDensoSH705xKline`)
- Create: `docs/denso-sh705x-kline-bench-checklist.md`
- Modify: `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md` (append a note)

- [ ] **Step 1: Flip the matrix row**

Replace the `FlashEcuSubaruDensoSH705xKline` row with (one line):

```markdown
| FlashEcuSubaruDensoSH705xKline | ECU | K-Line | read, test_write, write | yes | `subaru_denso_sh705x_kline_plan_test`, `subaru_denso_sh705x_kline_executor_test`, `test_flash_workflow` @ Wave 6b-2 | experimental | — | Exact protocol/MCU pairs `sub_ecu_denso_sh7055_04` / `_ecutek` / `_cobb` (SH7055, kernel `0xFFFF6004`) and `sub_ecu_denso_sh7058` / `_ecutek` / `_cobb` (SH7058, kernel `0xFFFF3000`); the transport column previously read "raw CAN" from cfg `<flash_transport>`, but this family only speaks K-Line. The adapter is reset before configuration through `IKlineFlashTransport::reset_connection()` (PR 6b-2a). TestWrite relies on the kernel's `FLASH_DISABLE` + `VALIDATE` dry run, whose ack is gated before any erase; see the [bench checklist](denso-sh705x-kline-bench-checklist.md). **Deliberate corrections:** Cobb Read/Write are rejected by the plan (legacy relied on cfg gating in the UI); the image must be exactly `romsize`; read pages must be complete with a valid sum8; ECU-ID, seed and CRC replies are length-checked before indexing; the post-upload baud change is checked; a Write whose post-compare still differs fails instead of reporting success. Progress reports bytes, not the legacy B/s estimate. |
```

- [ ] **Step 2: Write the bench checklist**

`docs/denso-sh705x-kline-bench-checklist.md`:

```markdown
# Subaru Denso SH705x K-Line bench checklist

## 0. STOP — not hardware-qualified

Do not treat this family as proven until each MCU (SH7055, SH7058) has passed
every section below on real hardware. Record adapter make, model, firmware,
interface mode, ECU part number and identifier, MCU, protocol variant, date,
operator and result. Keep separate records per MCU; evidence from one does not
qualify the other.

## 1. TestWrite safety premise — first, before any real write

- With a trusted full-ROM dump in hand, run TestWrite on an image that differs
  in one block. Capture the serial trace.
- Confirm the trace shows `FLASH_DISABLE` (0x21) acknowledged, then
  `BLANK_PAGE` (0x25), `WRITE_FLASH_BUFFER` (0x22) and `VALIDATE` (0x23) — and
  never `FLASH_ENABLE` (0x20) or `COMMIT` (0x24).
- Read the ROM again and confirm it is byte-identical to the trusted dump.
  If anything changed, stop: TestWrite is not a dry run on this kernel.

## 2. Startup

- Confirm the adapter reset happens before configuration, then 4800 baud.
- Cold start: the kernel probe at 62500 gets no reply, the SSM handshake
  (`BF`, `81`, `83`, `27 01`, `27 02`, `10 85 02`) succeeds, the kernel
  uploads at 15625 and answers its ID at 62500.
- Warm start with the kernel still running: the probe succeeds and no
  handshake or upload is sent.
- Repeat with an `_ecutek` protocol on an EcuTek-flashed ECU.

## 3. Read

- Read the full ROM and compare it byte-for-byte with a trusted image.
- Confirm the page reply layout the port requires: `BE EF`, length, `0x43`,
  0x400 data bytes, trailing sum8. If the kernel's trailing byte is not a
  sum8, record it and stop.
- Confirm the saved ROM ID is the ECU ID hex followed by `_`.

## 4. Write and recovery

- Write an image differing in one small block; confirm only that block is
  erased and rewritten and that the post-compare shows no differences.
- Interrupt power to the adapter mid-write on a sacrificial ECU, then confirm
  a warm-start write (kernel still running) recovers it.
```

- [ ] **Step 3: Note the wave in the parent spec**

Append to `docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md`:

```markdown
## Wave 6b-2 implementation note

The [Denso SH705x K-Line family spec](2026-09-24-step5-tail-wave6b2-denso-sh705x-kline-design.md)
records two approved departures from this design: the 6a-3/6a-4
behavior-correction exception applies instead of blanket preservation, and the
byte-identical seed-key and encrypt tables are shared with the EEPROM K-Line
executor in `denso_sh705x_kline_common.h` — data only, no protocol function.
Port item 2, `IKlineFlashTransport::reset_connection()`, landed in PR 6b-2a and
is called from `before_transport_configure()`. The drain moves from five
entries to four.
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
Expected: all green; 4 families remaining; clang-tidy reports no findings in changed files. Fix coverage gaps with tests, not exclusions.

- [ ] **Step 5: Commit**

```bash
git add docs/flash-qualification-matrix.md docs/denso-sh705x-kline-bench-checklist.md \
  docs/superpowers/specs/2026-09-19-step5-tail-wave6-singletons-design.md
git commit -m "docs(flash): record the Denso SH705x K-Line migration"
```

- [ ] **Step 6: Ask the user for approval to push, then open the PR**

After approval:
```bash
git push -u origin feat/wave6b2b-denso-sh705x-kline
gh pr create --title "feat(flash): migrate Subaru Denso SH705x K-Line (wave 6b-2b)" --body "$(cat <<'EOF'
### What
- portable plan and executor for the six exact `sub_ecu_denso_sh7055_04*` / `sub_ecu_denso_sh7058*` K-Line protocols: read, test write and write
- reset before configure through `IKlineFlashTransport::reset_connection()` (#<6b-2a PR>)
- route through the portable desktop workflow; remove the legacy dialog, operation and both MainWindow branches
- drain ratchet five -> four; matrix row `experimental`; bench checklist

### Why
- continue the step 5 legacy flash drain (wave 6b-2)
- corrections under the 6a-3/6a-4 policy: Cobb gated to TestWrite in the plan, exact ROM-size image, complete checksummed read pages, length-checked ECU-ID/seed/CRC replies, checked post-upload baud change, failed post-write verify is now an error

### Verification
- bazel test --config=release //...
- bazel build --config=release //:fastecu //:portable_closure
- prek run --all-files; bazel run //:clang_tidy_report_changed
- mutation checks for every correction and the TestWrite opcode selection

### References
- design: docs/superpowers/specs/2026-09-24-step5-tail-wave6b2-denso-sh705x-kline-design.md
- plan: docs/superpowers/plans/2026-09-24-step5-tail-wave6b2-denso-sh705x-kline.md
- hardware qualification blocked by docs/denso-sh705x-kline-bench-checklist.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
