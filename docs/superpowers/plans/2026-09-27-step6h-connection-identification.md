# Step 6h: Connection and SSM Identification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove `//src/ui/desktop:__pkg__` from the `serial_qt_compat` allowlist by moving `MainWindow`'s connection handling behind a platform `AdapterConnection` and its SSM ECU identification into a portable `identify_ssm_ecu` run on a worker thread, and delete the three unshipped developer toggles.

**Architecture:** Portable protocol logic (`identify_ssm_ecu`, SSM framing, ECU-ID parsing) lives in `//src/backend/diagnostics` and talks to the existing `IDiagnosticLink` port, which gains a parity field. The desktop platform adds `AdapterConnection` (a concrete Qt adapter over the facade, reached through `serial_platform_api`) and `SsmIdentifyWorker` (a `QThread` shaped like `DtcWorker`). `MainWindow` keeps presentation state, calls only the adapter, and passes the facade on as an opaque forward-declared reference.

**Tech Stack:** C++23, Qt 6, Bazel (pinned in `.bazelversion`), GoogleTest/GoogleMock, QtTest, `prek`, clang-tidy.

**Spec:** [docs/superpowers/specs/2026-09-27-step6h-connection-identification-design.md](../specs/2026-09-27-step6h-connection-identification-design.md). Read it before starting any task; this plan argues from it.

## Global Constraints

- Bazel is the only build graph. Every command is `bazel ... --config=release`.
- Backend operations return `fastecu::Result<T>` / `fastecu::Status`, checked with `.has_value()`, never `operator bool`. Exceptions never cross a port. Do not add an `ErrorKind` value.
- Pure protocol code uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView`; `QByteArray` appears only at Qt boundaries, converted through `src/algorithms/protocol/qt_compat/qt_bytes.h`.
- `src/backend/diagnostics` is portable: no Qt, no threads, no filesystem. New portable targets are registered in `bazel/portable_targets.bzl`.
- The `serial_qt_compat` visibility list and `FROZEN` in `scripts/check-serial-compat-allowlist.py` may only shrink. Never add an entry.
- The `qt_layer` package group in `bazel/qt/BUILD.bazel` may only shrink. The new package `//src/platform/desktop/common/connection` is already covered by `//src/platform/...`; do not edit `qt_layer`.
- Tests are co-located with the code. Portable tests use `fastecu_portable_gtest`; Qt tests use `fastecu_qttest` and a `main` that calls `::testing::InitGoogleMock` and fails on `::testing::Test::HasFailure()`.
- Mocks and fakes are package-owned (`src/backend/protocol/testing/fake_diagnostic_link.h`, `src/platform/desktop/common/serial/testing/fake_backend.h`).
- Platform differences go in BUILD-selected sources, never `#ifdef`. No new `#ifdef` in this step.
- Markdown cross-document references are links, not backticked paths.
- Git: one branch per PR, stacked; `prek` refuses commits on `master`. Every commit message ends with:

  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
  ```

- Push and open PRs only after the user authorizes it (Task 14).
- Nothing in this step is marked hardware-qualified. The bench checklist created in Task 13 starts with every row "Not yet tested".

## Review Focus

The inputs and conditions the spec implies but does not spell out, most likely to hurt a user first. Each one has a pinning test in the task named.

1. **No serial port is present** (the port list is empty): opening DTC, BIU, or the terminal must warn, not crash on `serial_ports.at(...)`. Pinned in Task 9 (`dtcWindowWithoutAPortWarnsInsteadOfCrashing`).
2. **The ECU never answers identification:** connect must finish after the five attempts, disconnect, and give the user their controls back. Pinned in Task 11 (`subaruConnectThatNeverAnswersDisconnectsAndRestoresControls`).
3. **The user disconnects while identification runs:** the worker must stop, and its late result must never be applied. Pinned in Task 11 (`disconnectDuringIdentificationCancelsAndDropsTheResult`).
4. **The line never goes quiet after an SSM1 answer:** the trailing drain must end. Pinned in Task 5 (`Ssm1DrainStopsAfterOneHundredReads`).
5. **An SSM2 answer with trailing bytes, or one from the wrong unit** (a TCU frame when the ECU was asked): trailing bytes are dropped, and a wrong target is `BadResponse`. Pinned in Task 4 (`KlineTrailingBytesAreDropped`, `KlineRejectsAnswerFromTheOtherUnit`).

---

## Stack layout

| PR | Branch | Parent | Tasks |
|---|---|---|---|
| 6h-0 | `docs/step6h-connection-identification` | `master` | spec + this plan (already on the branch) |
| 6h-1 | `refactor/step6h-1-dev-toggles` | 6h-0 | 1 |
| 6h-2 | `refactor/step6h-2-ssm-identify` | 6h-1 | 2–6 |
| 6h-3 | `refactor/step6h-3-adapter-connection` | 6h-2 | 7–9 |
| 6h-4 | `refactor/step6h-4-connect-worker` | 6h-3 | 10–13 |

Task 14 turns the branches into a GitHub stack with `gh stack`.

## File map

| File | Status | Responsibility |
|---|---|---|
| `src/algorithms/menu/menu_command.{h,cpp}`, `menu_command_test.cpp` | modify | drop the three dev-toggle commands |
| `src/ui/desktop/menu_actions.cpp`, `mainwindow.{h,cpp}` | modify | delete the toggles; route through the adapter; async connect |
| `src/backend/protocol/idiagnostic_link.h` | modify | `Parity` and `KlineLinkConfig::parity` |
| `src/backend/protocol/testing/fake_diagnostic_link.h`, `fake_diagnostic_link_test.cpp` | modify | record parity |
| `src/platform/desktop/common/diagnostics/serial_diagnostic_link.cpp`, `_test.cpp` | modify | apply parity |
| `src/backend/diagnostics/ssm_identify.{h,cpp}`, `ssm_identify_test.cpp` | create | portable SSM identification |
| `src/backend/diagnostics/BUILD.bazel`, `bazel/portable_targets.bzl` | modify | register `ssm_identify` |
| `src/platform/desktop/common/connection/adapter_connection.{h,cpp}`, `_test.cpp`, `BUILD.bazel` | create | the adapter |
| `src/platform/desktop/common/connection/testing/adapter_connection_harness.{h,cpp}`, `BUILD.bazel` | create | UI-test harness that hides the facade header |
| `src/ui/desktop/main_window_services.h` | modify | `serial` → `connection` |
| `apps/desktop/desktop_composition.{h,cpp}`, `desktop_composition_test.cpp`, `BUILD.bazel` | modify | build and own the adapter |
| `src/ui/desktop/mainwindow_test.cpp`, `src/ui/desktop/BUILD.bazel` | modify | harness, new tests, dependency changes |
| `src/platform/desktop/common/diagnostics/ssm_identify_worker.{h,cpp}`, `_test.cpp`, `BUILD.bazel` | create / modify | the worker |
| `src/ui/desktop/log_operations_ssm.cpp` | modify | legacy SSM init deleted; keeps `parse_log_value_list` and `log_to_file` |
| `src/platform/desktop/common/serial/BUILD.bazel`, `scripts/check-serial-compat-allowlist.py` | modify | drop `//src/ui/desktop:__pkg__` |
| `docs/design-notes.md`, `docs/modularization-plan.md`, `docs/tech-debt.md` | modify | close-out |
| `docs/connection-bench-checklist.md` | create | bench gate |

---

## PR 6h-1: delete the developer toggles

### Task 1: Delete `can_listener`, `simulate_obd`, and `test_haltech_ic7_display`

**Files:**
- Modify: `src/algorithms/menu/menu_command.h`, `src/algorithms/menu/menu_command.cpp`
- Test: `src/algorithms/menu/menu_command_test.cpp`
- Modify: `src/ui/desktop/menu_actions.cpp`, `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/mainwindow.h`

**Interfaces:**
- Consumes: nothing.
- Produces: `MenuCommand` without `HaltechIc7`, `SimulateObd`, `CanListener`. The ids `haltech_ic7`, `simulate_obd`, `can_listener` map to `MenuCommand::Unknown`, which `menu_action_triggered` already logs as "Unhandled menu action".

- [ ] **Step 1: Create the branch**

```bash
git switch docs/step6h-connection-identification
git switch -c refactor/step6h-1-dev-toggles
```

- [ ] **Step 2: Write the failing test**

Append to `src/algorithms/menu/menu_command_test.cpp`:

```cpp
// Step 6h deleted these developer toggles. A hand-edited menu.cfg that still
// names one must take the ordinary unknown-command path.
TEST(MenuCommandPortable, RetiredDeveloperTogglesMapToUnknown)
{
    for (const char *id : {"haltech_ic7", "simulate_obd", "can_listener"})
    {
        EXPECT_EQ(menu_command_from_id(id), MenuCommand::Unknown) << id;
    }
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `bazel test --config=release //src/algorithms/menu:menu_command_test`
Expected: FAIL. Each of the three ids maps to a real command today.

- [ ] **Step 4: Remove the commands**

In `src/algorithms/menu/menu_command.h`, delete these three enumerators:

```cpp
    HaltechIc7,
    SimulateObd,
    CanListener,
```

In `src/algorithms/menu/menu_command.cpp`, change `std::array<MenuCommandMapping, 37>` to `std::array<MenuCommandMapping, 34>` and delete these three rows:

```cpp
    {"haltech_ic7", MenuCommand::HaltechIc7},
    {"simulate_obd", MenuCommand::SimulateObd},
    {"can_listener", MenuCommand::CanListener},
```

- [ ] **Step 5: Delete the UI code**

In `src/ui/desktop/menu_actions.cpp`:
- delete these three `case` blocks in `menu_action_triggered`:

```cpp
    case MenuCommand::HaltechIc7:
        toggle_haltech_ic7_display();
        break;
    case MenuCommand::SimulateObd:
        toggle_simulate_obd();
        break;
    case MenuCommand::CanListener:
        toggle_can_listener();
        break;
```

- delete these whole function definitions: `MainWindow::toggle_haltech_ic7_display()`, `MainWindow::toggle_simulate_obd()`, `MainWindow::toggle_can_listener()`, `MainWindow::test_haltech_ic7_display()`, and `MainWindow::simulate_obd()`. Each runs from its signature line to its closing `}` at column 0.

In `src/ui/desktop/mainwindow.cpp`, delete the whole definition of `int MainWindow::can_listener()`.

In `src/ui/desktop/mainwindow.h`, delete these lines:

```cpp
    bool haltech_ic7_display_on = false;
    bool simulate_obd_on = false;
    bool can_listener_on = false;
```

```cpp
    void toggle_haltech_ic7_display();
    int test_haltech_ic7_display();
    void toggle_simulate_obd();
    void toggle_can_listener();
    int simulate_obd();
```

```cpp
    int can_listener();
```

- [ ] **Step 6: Confirm nothing else refers to them**

Run: `grep -rn "haltech\|simulate_obd\|can_listener\|HaltechIc7\|SimulateObd\|CanListener" src apps resources`
Expected: only the three ids in the new test in `menu_command_test.cpp`.

- [ ] **Step 7: Run the tests**

Run: `bazel test --config=release //src/algorithms/menu:all //src/backend/config:all //src/ui/desktop:all`
Expected: PASS. `//src/backend/config` holds `menu_definition_test`, which reads the shipped `menu.cfg`.

- [ ] **Step 8: Run the gates and commit**

Run: `bazel build --config=release //:fastecu && prek run --all-files`
Expected: both pass.

```bash
git add src/algorithms/menu src/ui/desktop/menu_actions.cpp src/ui/desktop/mainwindow.cpp src/ui/desktop/mainwindow.h
git commit -F - <<'EOF'
refactor: delete the unshipped developer toggles (step 6h-1)

can_listener, simulate_obd, and test_haltech_ic7_display were never in
the shipped menu.cfg and each ran an unbounded loop on the UI thread.
Their menu ids now take the unknown-command path.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

---

## PR 6h-2: parity and portable SSM identification

### Task 2: Parity in `KlineLinkConfig`

**Files:**
- Modify: `src/backend/protocol/idiagnostic_link.h`
- Modify: `src/backend/protocol/testing/fake_diagnostic_link.h`
- Test: `src/backend/protocol/testing/fake_diagnostic_link_test.cpp`
- Modify: `src/platform/desktop/common/diagnostics/serial_diagnostic_link.cpp`
- Test: `src/platform/desktop/common/diagnostics/serial_diagnostic_link_test.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class fastecu::diagnostics::Parity { None, Even }`, `constexpr std::string_view to_string(Parity)`, and `KlineLinkConfig::parity` (default `Parity::None`). `FakeDiagnosticLink` appends ` parity=Even` to its `open kline ...` call line when parity is `Even`, and nothing when it is `None`, so every existing expected call string stays valid. `SerialDiagnosticLink::open(KlineLinkConfig)` calls `set_serial_port_parity` immediately after `set_serial_port_baudrate`.

- [ ] **Step 1: Create the branch**

```bash
git switch -c refactor/step6h-2-ssm-identify
```

- [ ] **Step 2: Write the failing fake test**

Append inside the existing test file `src/backend/protocol/testing/fake_diagnostic_link_test.cpp` (it already has `using namespace fastecu::diagnostics;`):

```cpp
TEST(FakeDiagnosticLink, RecordsEvenParityAndStaysSilentForNone)
{
    FakeDiagnosticLink link;
    static_cast<void>(link.open(KlineLinkConfig{.baud = 1953, .parity = Parity::Even}));
    static_cast<void>(link.open(KlineLinkConfig{.baud = 4800}));
    EXPECT_EQ(link.calls.at(0), "open kline header=None iso14230=false baud=1953 start=00 tester=00 target=00 parity=Even");
    EXPECT_EQ(link.calls.at(1), "open kline header=None iso14230=false baud=4800 start=00 tester=00 target=00");
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `bazel test --config=release //src/backend/protocol/testing:fake_diagnostic_link_test`
Expected: FAIL to compile: `Parity` is not declared and `KlineLinkConfig` has no `parity` field.

- [ ] **Step 4: Add the field**

In `src/backend/protocol/idiagnostic_link.h`, after the `to_string(KlineHeader)` function, add:

```cpp
enum class Parity
{
    None,
    Even,
};

constexpr std::string_view to_string(Parity parity) noexcept
{
    return parity == Parity::Even ? "Even" : "None";
}
```

and in `struct KlineLinkConfig`, after `std::uint8_t target_id = 0;`, add:

```cpp
    Parity parity = Parity::None; // SSM1 runs 1953 8E1
```

In `src/backend/protocol/testing/fake_diagnostic_link.h`, replace the body of `open(const KlineLinkConfig& c)` with:

```cpp
        calls.push_back(std::format(
            "open kline header={} iso14230={} baud={} start={:02X} tester={:02X} target={:02X}{}", to_string(c.header),
            c.iso14230_connection, c.baud, c.start_byte, c.tester_id, c.target_id,
            c.parity == Parity::Even ? " parity=Even" : ""));
        return next(opens_);
```

- [ ] **Step 5: Run the fake test**

Run: `bazel test --config=release //src/backend/protocol/testing:fake_diagnostic_link_test //src/backend/diagnostics:all`
Expected: PASS, including `dtc_session_test`, whose expected `open kline ...` strings are unchanged.

- [ ] **Step 6: Write the failing adapter tests**

In `src/platform/desktop/common/diagnostics/serial_diagnostic_link_test.cpp`, add `#include <QSerialPort>` beside the other Qt includes and `using fastecu::diagnostics::Parity;` beside the other `using` lines. In `klineOpenResetsAppliesEverySetterThenOpens`, insert this expectation directly after the `set_serial_port_baudrate(QString("10400"))` line:

```cpp
            EXPECT_CALL(serial.fake(), set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)))
                .WillOnce(Return(true));
```

Then add a new slot after that test:

```cpp
    void evenParityIsAppliedBeforeTheOpen()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("1953"))).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::EvenParity)))
                .WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("ttyUSB0")));
        }
        SerialDiagnosticLink link(serial.get());
        QVERIFY(link.open(KlineLinkConfig{.baud = 1953, .parity = Parity::Even}).has_value());
    }
```

- [ ] **Step 7: Run them to verify they fail**

Run: `bazel test --config=release //src/platform/desktop/common/diagnostics:serial_diagnostic_link_test`
Expected: FAIL. `set_serial_port_parity` is never called, so both expectations are unsatisfied.

- [ ] **Step 8: Apply parity in the adapter**

In `src/platform/desktop/common/diagnostics/serial_diagnostic_link.cpp`, add `#include <QSerialPort>` and `#include <cstdint>` beside the existing includes, and in `SerialDiagnosticLink::open(const KlineLinkConfig& c)` insert this setter directly after the `set_serial_port_baudrate` entry:

```cpp
                    {"set_serial_port_parity",
                     [&]
                     {
                         return serial_->set_serial_port_parity(static_cast<std::uint8_t>(
                             c.parity == Parity::Even ? QSerialPort::EvenParity : QSerialPort::NoParity));
                     }},
```

- [ ] **Step 9: Run the tests**

Run: `bazel test --config=release //src/platform/desktop/common/diagnostics:all //src/ui/desktop:all`
Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add src/backend/protocol src/platform/desktop/common/diagnostics
git commit -F - <<'EOF'
feat(diagnostics): carry parity in the K-Line link config (step 6h-2)

SSM1 identification opens at 1953 8E1. KlineLinkConfig gains a parity
field, the fake records it, and SerialDiagnosticLink applies it right
after the baud rate, so every K-Line open now sets parity explicitly.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 3: SSM framing and ECU-ID helpers

**Files:**
- Create: `src/backend/diagnostics/ssm_identify.h`, `src/backend/diagnostics/ssm_identify.cpp`
- Test: `src/backend/diagnostics/ssm_identify_test.cpp`
- Modify: `src/backend/diagnostics/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `IDiagnosticLink`, `KlineLinkConfig`, `CanLinkConfig`, `Parity` (Task 2); `fastecu::IClock`, `fastecu::ICancellationToken`, `fastecu::Result`, `fastecu::fail`.
- Produces, in namespace `fastecu::diagnostics`:
  - `enum class SsmVariant { Ssm1, KlineSsm2, Iso15765Uds };`
  - `enum class SsmTarget { Ecu, Tcu };`
  - `struct SsmIdentifyRequest { SsmVariant variant = SsmVariant::KlineSsm2; SsmTarget target = SsmTarget::Ecu; };`
  - `struct SsmIdentity { std::string ecu_id; bytes::Bytes init_response; };`
  - `Result<SsmIdentity> identify_ssm_ecu(IDiagnosticLink&, IClock&, const ICancellationToken&, const SsmIdentifyRequest&);`. This task makes it return `Unsupported` for every variant; Tasks 4–6 fill it in.
  - `bytes::Bytes ssm_frame(bytes::ByteView payload, SsmTarget target);`
  - `std::optional<std::string> parse_ssm_ecu_id(bytes::ByteView init_response);`

- [ ] **Step 1: Write the header**

Create `src/backend/diagnostics/ssm_identify.h`:

```cpp
#pragma once
#include <optional>
#include <string>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/idiagnostic_link.h"

namespace fastecu::diagnostics
{

// Which legacy identification exchange to run. The toolbar's log transport
// picks it: "SSM" -> Ssm1, "K-Line" -> KlineSsm2, "iso15765" -> Iso15765Uds.
// Raw "CAN" never identified an ECU and has no variant.
enum class SsmVariant
{
    Ssm1,
    KlineSsm2,
    Iso15765Uds,
};

// The toolbar's ECU/TCU radio button.
enum class SsmTarget
{
    Ecu,
    Tcu,
};

struct SsmIdentifyRequest
{
    SsmVariant variant = SsmVariant::KlineSsm2;
    SsmTarget target = SsmTarget::Ecu;
};

struct SsmIdentity
{
    std::string ecu_id;         // uppercase hex, no separators
    bytes::Bytes init_response; // the init frame; empty for Iso15765Uds
};

// One identification attempt. Opens the link itself, sends the variant's
// request, and checks the answer; retry policy belongs to the caller.
// Timeout: nothing came back. BadResponse: something came back but is short
// or fails validation. Cancelled: the token tripped in a read or a sleep.
// Link errors pass through unchanged. Logs nothing.
Result<SsmIdentity> identify_ssm_ecu(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                     const SsmIdentifyRequest& request);

// 80 {10|18} F0 len payload... checksum, the checksum being the 8-bit sum of
// every byte before it.
bytes::Bytes ssm_frame(bytes::ByteView payload, SsmTarget target);

// The five ECU-ID bytes at offset 8 as uppercase hex, or nullopt when the
// frame is too short to hold them.
std::optional<std::string> parse_ssm_ecu_id(bytes::ByteView init_response);

} // namespace fastecu::diagnostics
```

- [ ] **Step 2: Write the failing tests**

Create `src/backend/diagnostics/ssm_identify_test.cpp`:

```cpp
#include "src/backend/diagnostics/ssm_identify.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using namespace fastecu::diagnostics;
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::FakeClock;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

namespace
{
bytes::Bytes b(std::initializer_list<int> values)
{
    bytes::Bytes out;
    for (int v : values)
    {
        out.push_back(static_cast<bytes::Byte>(v));
    }
    return out;
}

// Appends the SSM checksum, so a test can mutate one field of a frame
// without also having to recompute the last byte by hand.
bytes::Bytes with_checksum(bytes::Bytes frame)
{
    unsigned sum = 0;
    for (const bytes::Byte byte : frame)
    {
        sum += byte;
    }
    frame.push_back(static_cast<bytes::Byte>(sum & 0xFFU));
    return frame;
}

// RomRaider's documented ECU init response (io/protocol/ssm/iso9141/
// SSMProtocol.java, checkValidEcuInitResponse). ECU ID 3152584006.
const bytes::Bytes kRomRaiderEcuInit = b({
    0x80, 0xF0, 0x10, 0x39, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40,
    0x06, 0x73, 0xFA, 0xCB, 0x84, 0x2B, 0x83, 0xFE, 0xA8, 0x00, 0x00, 0x00,
    0x60, 0xCE, 0xD4, 0xFD, 0xB0, 0x60, 0x00, 0x0F, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xDC, 0x00, 0x00, 0x55, 0x1E, 0x30, 0xC0, 0xF2, 0x22, 0x00,
    0x00, 0x40, 0xFB, 0x00, 0xE1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x59,
});

// The shortest frame that still carries an ECU ID: 13 bytes plus checksum.
const bytes::Bytes kShortEcuInit = b({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C});
const bytes::Bytes kShortTcuInit = b({0x80, 0xF0, 0x18, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x74});

struct Harness
{
    FakeDiagnosticLink link;
    FakeClock clock;
    FakeCancellationToken token;

    fastecu::Result<SsmIdentity> run(SsmVariant variant, SsmTarget target = SsmTarget::Ecu)
    {
        return identify_ssm_ecu(link, clock, token, SsmIdentifyRequest{variant, target});
    }
};
} // namespace

TEST(SsmFrame, AddsHeaderLengthAndChecksum)
{
    EXPECT_EQ(ssm_frame(b({0xBF}), SsmTarget::Ecu), b({0x80, 0x10, 0xF0, 0x01, 0xBF, 0x40}));
    EXPECT_EQ(ssm_frame(b({0xBF}), SsmTarget::Tcu), b({0x80, 0x18, 0xF0, 0x01, 0xBF, 0x48}));
    EXPECT_EQ(ssm_frame(b({0xA8, 0x00, 0x00, 0x00, 0x08}), SsmTarget::Ecu),
              b({0x80, 0x10, 0xF0, 0x05, 0xA8, 0x00, 0x00, 0x00, 0x08, 0x35}));
}

TEST(ParseSsmEcuId, ReadsFiveBytesAtOffsetEight)
{
    EXPECT_EQ(parse_ssm_ecu_id(kRomRaiderEcuInit), std::optional<std::string>("3152584006"));
    EXPECT_EQ(parse_ssm_ecu_id(kShortEcuInit), std::optional<std::string>("3152584006"));
}

TEST(ParseSsmEcuId, RejectsFramesTooShortForAnId)
{
    const bytes::Bytes twelve(kShortEcuInit.begin(), kShortEcuInit.begin() + 12);
    const bytes::Bytes thirteen(kShortEcuInit.begin(), kShortEcuInit.begin() + 13);
    EXPECT_EQ(parse_ssm_ecu_id(twelve), std::nullopt);
    EXPECT_EQ(parse_ssm_ecu_id(thirteen), std::optional<std::string>("3152584006"));
}
```

- [ ] **Step 3: Register the targets**

In `src/backend/diagnostics/BUILD.bazel`, change the visibility comment `# Step 6g's dialogs (DtcOperations et al.) are the only consumer.` to `# Step 6g's dialogs and step 6h's MainWindow identification.`, and append:

```python
cc_library(
    name = "ssm_identify",
    srcs = ["ssm_identify.cpp"],
    hdrs = ["ssm_identify.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/backend/ports",
        "//src/backend/protocol",
    ],
)

fastecu_portable_gtest(
    name = "ssm_identify_test",
    srcs = ["ssm_identify_test.cpp"],
    deps = [
        ":ssm_identify",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:result_matchers",
        "//src/backend/protocol/testing:fake_diagnostic_link",
    ],
)
```

In `bazel/portable_targets.bzl`, change the `src/backend/diagnostics` entry to:

```python
    "src/backend/diagnostics": [
        "dtc_session",
        "obd_frames",
        "ssm_identify",
    ],
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/diagnostics:ssm_identify_test`
Expected: FAIL to build: `ssm_identify.cpp` does not exist.

- [ ] **Step 5: Implement the helpers**

Create `src/backend/diagnostics/ssm_identify.cpp`:

```cpp
#include "src/backend/diagnostics/ssm_identify.h"

#include <cstddef>
#include <format>
#include <string>

namespace fastecu::diagnostics
{
namespace
{

constexpr bytes::Byte kSsmHeader = 0x80;
constexpr bytes::Byte kSsmTester = 0xF0;
constexpr std::size_t kEcuIdOffset = 8;
constexpr std::size_t kEcuIdLength = 5;

bytes::Byte target_id(SsmTarget target)
{
    return target == SsmTarget::Ecu ? 0x10 : 0x18;
}

bytes::Byte ssm_checksum(bytes::ByteView data)
{
    unsigned sum = 0;
    for (const bytes::Byte byte : data)
    {
        sum += byte;
    }
    return static_cast<bytes::Byte>(sum & 0xFFU);
}

std::string upper_hex(bytes::ByteView data)
{
    std::string out;
    for (const bytes::Byte byte : data)
    {
        out += std::format("{:02X}", byte);
    }
    return out;
}

} // namespace

bytes::Bytes ssm_frame(bytes::ByteView payload, SsmTarget target)
{
    bytes::Bytes frame{kSsmHeader, target_id(target), kSsmTester, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(ssm_checksum(frame));
    return frame;
}

std::optional<std::string> parse_ssm_ecu_id(bytes::ByteView init_response)
{
    if (init_response.size() < kEcuIdOffset + kEcuIdLength)
    {
        return std::nullopt;
    }
    return upper_hex(init_response.subspan(kEcuIdOffset, kEcuIdLength));
}

Result<SsmIdentity> identify_ssm_ecu(IDiagnosticLink& /*link*/, IClock& /*clock*/,
                                     const ICancellationToken& /*cancellation*/, const SsmIdentifyRequest& /*request*/)
{
    return fail(ErrorKind::Unsupported, "SSM identification is not implemented yet");
}

} // namespace fastecu::diagnostics
```

- [ ] **Step 6: Run the tests and the closure guard**

Run: `bazel test --config=release //src/backend/diagnostics:ssm_identify_test && bazel build --config=release //:portable_closure`
Expected: 3 tests PASS; the closure guard builds.

- [ ] **Step 7: Commit**

```bash
git add src/backend/diagnostics bazel/portable_targets.bzl
git commit -F - <<'EOF'
feat(diagnostics): add portable SSM framing and ECU-ID parsing (step 6h-2)

ssm_frame and parse_ssm_ecu_id replace MainWindow's add_ssm_header,
calculate_checksum, and parse_ecuid. identify_ssm_ecu is declared and
registered as a portable target; its variants follow.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 4: K-Line SSM2 identification with RomRaider-style validation

**Files:**
- Modify: `src/backend/diagnostics/ssm_identify.cpp`
- Test: `src/backend/diagnostics/ssm_identify_test.cpp`

**Interfaces:**
- Consumes: Task 3's declarations; `FakeDiagnosticLink` call lines (`"open kline ..."`, `"write XX XX"`, `"read N"`).
- Produces: `identify_ssm_ecu` handling `SsmVariant::KlineSsm2`. The file-local helpers `read_into` and `check_open`/`check_write` are reused by Tasks 5 and 6.

- [ ] **Step 1: Write the failing tests**

Append to `src/backend/diagnostics/ssm_identify_test.cpp`:

```cpp
namespace
{
const std::string kKlineOpen = "open kline header=None iso14230=false baud=4800 start=00 tester=00 target=00";
}

TEST(IdentifyKlineSsm2, SendsTheInitRequestAndReturnsTheEcuId)
{
    Harness h;
    h.link.queue_read(kRomRaiderEcuInit);
    const auto result = h.run(SsmVariant::KlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kRomRaiderEcuInit);
    EXPECT_THAT(h.link.calls, ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40", "read 200"));
    EXPECT_EQ(h.clock.elapsed(), 200ms);
}

TEST(IdentifyKlineSsm2, TcuIsAddressedAsEighteen)
{
    Harness h;
    h.link.queue_read(kShortTcuInit);
    ASSERT_THAT(h.run(SsmVariant::KlineSsm2, SsmTarget::Tcu), IsOk());
    EXPECT_EQ(h.link.calls.at(1), "write 80 18 F0 01 BF 48");
}

TEST(IdentifyKlineSsm2, AssemblesTheFrameFromPartialReads)
{
    Harness h;
    h.link.queue_read(b({0x80, 0xF0}));
    h.link.queue_read(b({0x10, 0x09, 0xFF}));
    h.link.queue_read(b({0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06}));
    h.link.queue_read(b({0x6C}));
    const auto result = h.run(SsmVariant::KlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
    EXPECT_THAT(h.link.calls, ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40", "read 200", "read 50", "read 50",
                                          "read 50"));
}

TEST(IdentifyKlineSsm2, NoAnswerIsTimeoutAfterTheLegacyReadBudget)
{
    Harness h;
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErr(ErrorKind::Timeout));
    // open, write, one 200 ms read, then ten 50 ms reads waiting for a header.
    EXPECT_EQ(h.link.calls.size(), 13U);
    EXPECT_EQ(h.link.calls.back(), "read 50");
}

TEST(IdentifyKlineSsm2, IncompleteHeaderIsBadResponse)
{
    Harness h;
    h.link.queue_read(b({0x80, 0xF0}));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("header incomplete")));
}

TEST(IdentifyKlineSsm2, TruncatedBodyIsBadResponse)
{
    Harness h;
    h.link.queue_read(b({0x80, 0xF0, 0x10, 0x09, 0xFF}));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("truncated")));
    // The header was already complete, so all ten 50 ms reads wait for the body.
    EXPECT_EQ(h.link.calls.size(), 13U);
}

TEST(IdentifyKlineSsm2, KlineTrailingBytesAreDropped)
{
    Harness h;
    bytes::Bytes with_tail = kShortEcuInit;
    with_tail.push_back(0xAA);
    with_tail.push_back(0xBB);
    h.link.queue_read(with_tail);
    const auto result = h.run(SsmVariant::KlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

TEST(IdentifyKlineSsm2, RejectsAZeroLengthFrame)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x80, 0xF0, 0x10, 0x00})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("no response code")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongHeaderByte)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x81, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("header byte")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongTesterId)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x80, 0xF1, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("tester id")));
}

TEST(IdentifyKlineSsm2, KlineRejectsAnswerFromTheOtherUnit)
{
    Harness h;
    h.link.queue_read(kShortTcuInit); // the ECU was asked
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2, SsmTarget::Ecu),
                IsErrWith(ErrorKind::BadResponse, HasSubstr("target id")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongResponseCode)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x80, 0xF0, 0x10, 0x09, 0xE8, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("response code is E8")));
}

TEST(IdentifyKlineSsm2, RejectsABadChecksum)
{
    Harness h;
    bytes::Bytes corrupt = kShortEcuInit;
    corrupt.back() ^= 0x01;
    h.link.queue_read(corrupt);
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("checksum")));
}

TEST(IdentifyKlineSsm2, AValidFrameTooShortForAnIdIsBadResponse)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x80, 0xF0, 0x10, 0x02, 0xFF, 0x11})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("ECU ID")));
}

TEST(IdentifyKlineSsm2, CancelledSleepIsCancelled)
{
    Harness h;
    h.token.set_cancelled(true);
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErr(ErrorKind::Cancelled));
    EXPECT_THAT(h.link.calls, ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40"));
}

TEST(IdentifyKlineSsm2, LinkErrorsPassThrough)
{
    Harness h;
    h.link.queue_read_error(ErrorKind::Disconnected);
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErr(ErrorKind::Disconnected));

    Harness opening;
    opening.link.queue_open(fastecu::fail(ErrorKind::Disconnected, "no port"));
    EXPECT_THAT(opening.run(SsmVariant::KlineSsm2), IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(opening.link.calls.size(), 1U);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test --config=release //src/backend/diagnostics:ssm_identify_test`
Expected: FAIL. Every `IdentifyKlineSsm2` test gets `Unsupported`.

- [ ] **Step 3: Implement the K-Line variant**

In `src/backend/diagnostics/ssm_identify.cpp`, add `#include <array>`, `#include <chrono>`, and `#include <utility>` beside the other includes. Inside the anonymous namespace, after `upper_hex`, add:

```cpp
using namespace std::chrono_literals;

constexpr bytes::Byte kSsmInitCommand = 0xBF;
constexpr bytes::Byte kSsmInitResponse = 0xFF;
constexpr std::size_t kSsmFrameOverhead = 5; // header, tester, target, length, checksum

std::string spaced_hex(bytes::ByteView data)
{
    std::string out;
    for (const bytes::Byte byte : data)
    {
        out += std::format("{}{:02X}", out.empty() ? "" : " ", byte);
    }
    return out;
}

// Appends one read to `frame`. true when bytes arrived, false on a deadline.
Result<bool> read_into(IDiagnosticLink& link, bytes::Bytes& frame, std::chrono::milliseconds timeout,
                       const ICancellationToken& cancellation)
{
    auto read = link.read(timeout, cancellation);
    if (!read.has_value())
    {
        return std::unexpected(read.error());
    }
    if (!read->has_value())
    {
        return false;
    }
    frame.insert(frame.end(), (*read)->begin(), (*read)->end());
    return true;
}

Status check_written(IDiagnosticLink& link, bytes::ByteView request)
{
    auto written = link.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return {};
}

std::unexpected<Error> bad_frame(const std::string& what, bytes::ByteView frame)
{
    return fail(ErrorKind::BadResponse, "SSM init response " + what + ": " + spaced_hex(frame));
}

// RomRaider's SSMResponseProcessor.validateResponse, applied to a frame
// already cut to its declared length.
Status validate_ssm2_init(bytes::ByteView frame, SsmTarget target)
{
    if (frame[3] == 0)
    {
        return bad_frame("has no response code", frame);
    }
    if (frame[0] != kSsmHeader)
    {
        return bad_frame(std::format("header byte is {:02X}, expected {:02X}", frame[0], kSsmHeader), frame);
    }
    if (frame[1] != kSsmTester)
    {
        return bad_frame(std::format("tester id is {:02X}, expected {:02X}", frame[1], kSsmTester), frame);
    }
    if (frame[2] != target_id(target))
    {
        return bad_frame(std::format("target id is {:02X}, expected {:02X}", frame[2], target_id(target)), frame);
    }
    if (frame[4] != kSsmInitResponse)
    {
        return bad_frame(std::format("response code is {:02X}, expected {:02X}", frame[4], kSsmInitResponse), frame);
    }
    const bytes::Byte expected = ssm_checksum(frame.first(frame.size() - 1));
    if (frame.back() != expected)
    {
        return bad_frame(std::format("checksum is {:02X}, expected {:02X}", frame.back(), expected), frame);
    }
    return {};
}

Result<SsmIdentity> identify_kline_ssm2(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                        SsmTarget target)
{
    // The state ssm_kline_init inherited from connect_to_ecu and
    // log_transport_changed, now set explicitly.
    if (auto opened = link.open(KlineLinkConfig{.header = KlineHeader::None, .baud = 4800, .parity = Parity::None});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    if (auto written = check_written(link, ssm_frame(std::array<bytes::Byte, 1>{kSsmInitCommand}, target));
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (auto slept = clock.sleep(200ms, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }

    bytes::Bytes frame;
    if (auto got = read_into(link, frame, 200ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
    for (int attempt = 0; frame.size() < 4 && attempt < 10; ++attempt)
    {
        if (auto got = read_into(link, frame, 50ms, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    if (frame.empty())
    {
        return fail(ErrorKind::Timeout, "no SSM init response");
    }
    if (frame.size() < 4)
    {
        return bad_frame("header incomplete", frame);
    }
    const std::size_t declared = frame[3] + kSsmFrameOverhead;
    for (int attempt = 0; frame.size() < declared && attempt < 10; ++attempt)
    {
        if (auto got = read_into(link, frame, 50ms, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    if (frame.size() < declared)
    {
        return bad_frame("truncated", frame);
    }
    frame.resize(declared); // the legacy code accepted len >= declared

    if (auto valid = validate_ssm2_init(frame, target); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    auto ecu_id = parse_ssm_ecu_id(frame);
    if (!ecu_id.has_value())
    {
        return bad_frame("is too short for an ECU ID", frame);
    }
    return SsmIdentity{std::move(*ecu_id), std::move(frame)};
}
```

Replace the stub `identify_ssm_ecu` with:

```cpp
Result<SsmIdentity> identify_ssm_ecu(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                     const SsmIdentifyRequest& request)
{
    switch (request.variant)
    {
    case SsmVariant::KlineSsm2:
        return identify_kline_ssm2(link, clock, cancellation, request.target);
    case SsmVariant::Ssm1:
    case SsmVariant::Iso15765Uds:
        break;
    }
    return fail(ErrorKind::Unsupported, "SSM identification variant is not implemented yet");
}
```

- [ ] **Step 4: Run the tests**

Run: `bazel test --config=release //src/backend/diagnostics:ssm_identify_test`
Expected: PASS.

- [ ] **Step 5: Mutation-check each validation**

The design notes require every correction to be pinned by a mutation check. For each of the six `if` checks in `validate_ssm2_init`, and for the `frame.resize(declared)` line, do this in turn: comment the check out, run `bazel test --config=release //src/backend/diagnostics:ssm_identify_test`, confirm that exactly the matching test fails (`RejectsAZeroLengthFrame`, `RejectsAWrongHeaderByte`, `RejectsAWrongTesterId`, `KlineRejectsAnswerFromTheOtherUnit`, `RejectsAWrongResponseCode`, `RejectsABadChecksum`, `KlineTrailingBytesAreDropped`), and then restore it. Record the seven outcomes in the PR description.

- [ ] **Step 6: Commit**

```bash
git add src/backend/diagnostics/ssm_identify.cpp src/backend/diagnostics/ssm_identify_test.cpp
git commit -F - <<'EOF'
feat(diagnostics): identify K-Line SSM2 ECUs portably (step 6h-2)

Reproduces ssm_kline_init's request and read budget, opens the link with
the settings it used to inherit, and validates the init response the way
RomRaider does: header, tester, target, response code, and checksum, on
a frame cut to its declared length.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 5: SSM1 identification

**Files:**
- Modify: `src/backend/diagnostics/ssm_identify.cpp`
- Test: `src/backend/diagnostics/ssm_identify_test.cpp`

**Interfaces:**
- Consumes: Task 4's `read_into`, `check_written`, `bad_frame`, `spaced_hex`.
- Produces: `identify_ssm_ecu` handling `SsmVariant::Ssm1`.

- [ ] **Step 1: Write the failing tests**

Append to `src/backend/diagnostics/ssm_identify_test.cpp`:

```cpp
namespace
{
const std::string kSsm1Open = "open kline header=None iso14230=false baud=1953 start=00 tester=00 target=00 parity=Even";

// The 12 reads that follow the two wake-up writes, all silent.
void queue_silent_wakeup(FakeDiagnosticLink& link)
{
    for (int i = 0; i < 12; ++i)
    {
        link.queue_no_frame();
    }
}

std::vector<std::string> ssm1_calls_through_first_frame()
{
    std::vector<std::string> calls{kSsm1Open, "write 78 12 34 00"};
    for (int i = 0; i < 10; ++i)
    {
        calls.emplace_back("read 500");
    }
    calls.emplace_back("write 00 46 48 49");
    calls.emplace_back("read 500");
    calls.emplace_back("read 500");
    calls.emplace_back("write 12 00 00 00");
    calls.emplace_back("read 500");
    return calls;
}
} // namespace

TEST(IdentifySsm1, SendsTheSsm1SequenceAtEvenParity)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    const auto result = h.run(SsmVariant::Ssm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kShortEcuInit);
    auto expected = ssm1_calls_through_first_frame();
    expected.emplace_back("read 100"); // the trailing read comes back empty
    EXPECT_EQ(h.link.calls, expected);
}

TEST(IdentifySsm1, WakeupResponsesAreNotPartOfTheFrame)
{
    Harness h;
    h.link.queue_read(b({0x01, 0x02}));
    for (int i = 0; i < 11; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(kShortEcuInit);
    const auto result = h.run(SsmVariant::Ssm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

// Pinned: SSM1 is checked by length only. RomRaider's checks are SSM2's.
TEST(IdentifySsm1, OnlyTheLengthIsChecked)
{
    Harness h;
    queue_silent_wakeup(h.link);
    const bytes::Bytes odd = b({0x12, 0x34, 0x56, 0x09, 0x00, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x00});
    h.link.queue_read(odd);
    ASSERT_THAT(h.run(SsmVariant::Ssm1), IsOk());
}

TEST(IdentifySsm1, LengthMismatchIsBadResponse)
{
    Harness h;
    queue_silent_wakeup(h.link);
    bytes::Bytes mismatch = kShortEcuInit;
    mismatch[3] = 0x0A;
    h.link.queue_read(mismatch);
    EXPECT_THAT(h.run(SsmVariant::Ssm1), IsErr(ErrorKind::BadResponse));
}

TEST(IdentifySsm1, ShortFrameIsBadResponseNotAnOutOfRangeRead)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(b({0x80, 0xF0}));
    EXPECT_THAT(h.run(SsmVariant::Ssm1), IsErr(ErrorKind::BadResponse));
}

TEST(IdentifySsm1, NoAnswerIsTimeout)
{
    Harness h;
    EXPECT_THAT(h.run(SsmVariant::Ssm1), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(h.link.calls, ssm1_calls_through_first_frame());
}

TEST(IdentifySsm1, ATrailingFrameWithAnIdReplacesTheFirst)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    const bytes::Bytes second = b({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x01, 0x02, 0x03, 0x04, 0x05, 0x5A});
    h.link.queue_read(second);
    const auto result = h.run(SsmVariant::Ssm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "0102030405");
    EXPECT_EQ(result->init_response, second);
    auto expected = ssm1_calls_through_first_frame();
    expected.emplace_back("read 100"); // the second frame
    expected.emplace_back("read 100"); // the drain, empty
    EXPECT_EQ(h.link.calls, expected);
}

TEST(IdentifySsm1, ATrailingFrameTooShortForAnIdIsIgnored)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    h.link.queue_read(b({0x01, 0x02}));
    const auto result = h.run(SsmVariant::Ssm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

TEST(IdentifySsm1, Ssm1DrainStopsAfterOneHundredReads)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    for (int i = 0; i < 150; ++i)
    {
        h.link.queue_read(b({0x55}));
    }
    ASSERT_THAT(h.run(SsmVariant::Ssm1), IsOk());
    const auto trailing = std::count(h.link.calls.begin(), h.link.calls.end(), std::string("read 100"));
    EXPECT_EQ(trailing, 101); // one trailing frame, then at most 100 drain reads
}

TEST(IdentifySsm1, CancellingDuringTheDrainIsCancelled)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    for (int i = 0; i < 150; ++i)
    {
        h.link.queue_read(b({0x55}));
    }
    // 18 calls reach the first trailing read; cancel a few reads into the drain.
    h.token.set_predicate([&h] { return h.link.calls.size() > 22; });
    EXPECT_THAT(h.run(SsmVariant::Ssm1), IsErr(ErrorKind::Cancelled));
}
```

Add `#include <algorithm>` to the test's includes for `std::count`.

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test --config=release //src/backend/diagnostics:ssm_identify_test`
Expected: FAIL. Every `IdentifySsm1` test gets `Unsupported`.

- [ ] **Step 3: Implement the SSM1 variant**

In the anonymous namespace of `src/backend/diagnostics/ssm_identify.cpp`, after `identify_kline_ssm2`, add:

```cpp
constexpr int kSsm1MaxDrainReads = 100;

// Writes `request`, then reads `count` times for `timeout`, discarding what
// arrives: ssm_init logged these bytes and never used them.
Status write_then_discard(IDiagnosticLink& link, bytes::ByteView request, int count, std::chrono::milliseconds timeout,
                          const ICancellationToken& cancellation)
{
    if (auto written = check_written(link, request); !written.has_value())
    {
        return written;
    }
    bytes::Bytes discarded;
    for (int i = 0; i < count; ++i)
    {
        if (auto got = read_into(link, discarded, timeout, cancellation); !got.has_value())
        {
            return std::unexpected(got.error());
        }
    }
    return {};
}

Result<SsmIdentity> identify_ssm1(IDiagnosticLink& link, const ICancellationToken& cancellation)
{
    if (auto opened = link.open(KlineLinkConfig{.header = KlineHeader::None, .baud = 1953, .parity = Parity::Even});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    // Every write is echo-checked: IDiagnosticLink has no other kind. The
    // legacy ssm_init sent the second and third without the echo check.
    if (auto woken = write_then_discard(link, std::array<bytes::Byte, 4>{0x78, 0x12, 0x34, 0x00}, 10, 500ms,
                                        cancellation);
        !woken.has_value())
    {
        return std::unexpected(woken.error());
    }
    if (auto woken = write_then_discard(link, std::array<bytes::Byte, 4>{0x00, 0x46, 0x48, 0x49}, 2, 500ms,
                                        cancellation);
        !woken.has_value())
    {
        return std::unexpected(woken.error());
    }
    if (auto written = check_written(link, std::array<bytes::Byte, 4>{0x12, 0x00, 0x00, 0x00}); !written.has_value())
    {
        return std::unexpected(written.error());
    }

    bytes::Bytes frame;
    if (auto got = read_into(link, frame, 500ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
    if (frame.empty())
    {
        return fail(ErrorKind::Timeout, "no SSM1 init response");
    }
    // Pinned: the length is the only check on SSM1.
    if (frame.size() < 4 || frame.size() != frame[3] + kSsmFrameOverhead)
    {
        return bad_frame("length does not match its length byte", frame);
    }
    auto ecu_id = parse_ssm_ecu_id(frame);
    if (!ecu_id.has_value())
    {
        return bad_frame("is too short for an ECU ID", frame);
    }
    SsmIdentity identity{std::move(*ecu_id), std::move(frame)};

    // ssm_init parsed at most one more frame, unchecked, then drained.
    bytes::Bytes trailing;
    auto got = read_into(link, trailing, 100ms, cancellation);
    if (!got.has_value())
    {
        return std::unexpected(got.error());
    }
    if (!*got)
    {
        return identity;
    }
    if (auto trailing_id = parse_ssm_ecu_id(trailing); trailing_id.has_value())
    {
        identity = SsmIdentity{std::move(*trailing_id), std::move(trailing)};
    }
    for (int i = 0; i < kSsm1MaxDrainReads; ++i)
    {
        bytes::Bytes discarded;
        auto drained = read_into(link, discarded, 100ms, cancellation);
        if (!drained.has_value())
        {
            return std::unexpected(drained.error());
        }
        if (!*drained)
        {
            break;
        }
    }
    return identity;
}
```

In `identify_ssm_ecu`, route `SsmVariant::Ssm1` to it:

```cpp
    case SsmVariant::Ssm1:
        return identify_ssm1(link, cancellation);
    case SsmVariant::Iso15765Uds:
        break;
```

- [ ] **Step 4: Run the tests**

Run: `bazel test --config=release //src/backend/diagnostics:ssm_identify_test`
Expected: PASS.

- [ ] **Step 5: Mutation-check the drain cap**

Change `kSsm1MaxDrainReads` to `1000`, run the test target, and confirm `Ssm1DrainStopsAfterOneHundredReads` fails. Then restore it.

- [ ] **Step 6: Commit**

```bash
git add src/backend/diagnostics/ssm_identify.cpp src/backend/diagnostics/ssm_identify_test.cpp
git commit -F - <<'EOF'
feat(diagnostics): identify SSM1 ECUs portably (step 6h-2)

Reproduces ssm_init's wake-up sequence at 1953 8E1 with echo-checked
writes, keeps its length-only check and one-extra-frame parse, and caps
the trailing drain at 100 reads so a line that never goes quiet cannot
hang the connect.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 6: iso15765 identification, then close PR 6h-2

**Files:**
- Modify: `src/backend/diagnostics/ssm_identify.cpp`
- Test: `src/backend/diagnostics/ssm_identify_test.cpp`

**Interfaces:**
- Consumes: Task 4's helpers.
- Produces: `identify_ssm_ecu` handling every `SsmVariant`; no `Unsupported` path remains.

- [ ] **Step 1: Write the failing tests**

Append to `src/backend/diagnostics/ssm_identify_test.cpp`:

```cpp
TEST(IdentifyIso15765Uds, ReadsF182FromTheEcu)
{
    Harness h;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x12, 0x34, 0x56, 0x78, 0x9A}));
    const auto result = h.run(SsmVariant::Iso15765Uds);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "123456789A");
    EXPECT_THAT(result->init_response, IsEmpty());
    EXPECT_THAT(h.link.calls,
                ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "write 00 00 07 E0 22 F1 82", "read 100"));
}

TEST(IdentifyIso15765Uds, TcuIsAddressedAs7E1)
{
    Harness h;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x01}));
    ASSERT_THAT(h.run(SsmVariant::Iso15765Uds, SsmTarget::Tcu), IsOk());
    EXPECT_EQ(h.link.calls.at(0), "open can iso15765=true bitrate=500000 extended=false source=7E1 destination=7E8");
    EXPECT_EQ(h.link.calls.at(1), "write 00 00 07 E1 22 F1 82");
}

TEST(IdentifyIso15765Uds, NegativeResponseIsBadResponse)
{
    Harness h;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x7F, 0x22, 0x31}));
    EXPECT_THAT(h.run(SsmVariant::Iso15765Uds), IsErr(ErrorKind::BadResponse));
}

TEST(IdentifyIso15765Uds, AnAnswerWithNoIdBytesIsBadResponse)
{
    Harness h;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82}));
    EXPECT_THAT(h.run(SsmVariant::Iso15765Uds), IsErr(ErrorKind::BadResponse));
}

TEST(IdentifyIso15765Uds, NoAnswerIsTimeout)
{
    Harness h;
    EXPECT_THAT(h.run(SsmVariant::Iso15765Uds), IsErr(ErrorKind::Timeout));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test --config=release //src/backend/diagnostics:ssm_identify_test`
Expected: FAIL. The `IdentifyIso15765Uds` tests get `Unsupported`.

- [ ] **Step 3: Implement the variant**

In the anonymous namespace, after `identify_ssm1`, add:

```cpp
Result<SsmIdentity> identify_iso15765_uds(IDiagnosticLink& link, const ICancellationToken& cancellation,
                                          SsmTarget target)
{
    const std::uint32_t source = target == SsmTarget::Ecu ? 0x7E0 : 0x7E1;
    if (auto opened = link.open(CanLinkConfig{.iso15765 = true,
                                              .bitrate = 500000,
                                              .extended_id = false,
                                              .source_id = source,
                                              .destination_id = 0x7E8});
        !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    const std::array<bytes::Byte, 7> request{
        0x00, 0x00, static_cast<bytes::Byte>((source >> 8U) & 0xFFU), static_cast<bytes::Byte>(source & 0xFFU),
        0x22, 0xF1, 0x82};
    if (auto written = check_written(link, request); !written.has_value())
    {
        return std::unexpected(written.error());
    }

    bytes::Bytes frame;
    if (auto got = read_into(link, frame, 100ms, cancellation); !got.has_value())
    {
        return std::unexpected(got.error());
    }
    if (frame.empty())
    {
        return fail(ErrorKind::Timeout, "no answer to ReadDataByIdentifier F182");
    }
    if (frame.size() <= 7 || frame[4] != 0x62 || frame[5] != 0xF1 || frame[6] != 0x82)
    {
        return fail(ErrorKind::BadResponse, "unexpected answer to ReadDataByIdentifier F182: " + spaced_hex(frame));
    }
    return SsmIdentity{upper_hex(bytes::ByteView(frame).subspan(7)), {}};
}
```

Add `#include <cstdint>` to the includes, and make `identify_ssm_ecu`'s switch complete. Replace the whole function with:

```cpp
Result<SsmIdentity> identify_ssm_ecu(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                     const SsmIdentifyRequest& request)
{
    switch (request.variant)
    {
    case SsmVariant::Ssm1:
        return identify_ssm1(link, cancellation);
    case SsmVariant::KlineSsm2:
        return identify_kline_ssm2(link, clock, cancellation, request.target);
    case SsmVariant::Iso15765Uds:
        return identify_iso15765_uds(link, cancellation, request.target);
    }
    return fail(ErrorKind::Internal, "unknown SSM identification variant");
}
```

- [ ] **Step 4: Run the PR's tests and gates**

Run:

```bash
bazel test --config=release //src/backend/diagnostics:all //src/backend/protocol/...:all //src/platform/desktop/common/diagnostics:all
bazel build --config=release //:fastecu //:portable_closure
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Expected: every command passes. clang-tidy reports no findings in the changed translation units.

- [ ] **Step 5: Commit**

```bash
git add src/backend/diagnostics/ssm_identify.cpp src/backend/diagnostics/ssm_identify_test.cpp
git commit -F - <<'EOF'
feat(diagnostics): identify iso15765 ECUs by DID F182 (step 6h-2)

Reproduces ssm_can_init's ReadDataByIdentifier F182 exchange at
0x7E0/0x7E1 -> 0x7E8. identify_ssm_ecu now covers every variant the
toolbar can select.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

---

## PR 6h-3: the adapter connection

### Task 7: `AdapterConnection`

**Files:**
- Create: `src/platform/desktop/common/connection/BUILD.bazel`
- Create: `src/platform/desktop/common/connection/adapter_connection.h`, `adapter_connection.cpp`
- Test: `src/platform/desktop/common/connection/adapter_connection_test.cpp`

**Interfaces:**
- Consumes: `SerialPortActions` through `//src/platform/desktop/common/serial:serial_platform_api`.
- Produces, in namespace `fastecu::desktop::connection`:
  - `enum class LogTransport { Can, Iso15765, KLine, Ssm, Other };`
  - `LogTransport log_transport_from_text(const QString& text);`
  - `class AdapterConnection : public QObject`, constructed from `SerialPortActions&`, with `available_ports()`, `set_initial_port(const QString&, const QString&)`, `select_port(const QString&)`, `QString open()`, `bool is_open()`, `reset()`, `apply_log_transport(LogTransport, bool ssm_protocol)`, `clear_link_flags()`, `return_to_idle()`, `set_port_speed(int)`, `std::optional<unsigned long> battery_millivolts()`, `wait_for_source()`, `SerialPortActions& facade()`, and the signal `stateChanged(QRemoteObjectReplica::State, QRemoteObjectReplica::State)`.

- [ ] **Step 1: Create the branch**

```bash
git switch -c refactor/step6h-3-adapter-connection
```

- [ ] **Step 2: Write the header**

Create `src/platform/desktop/common/connection/adapter_connection.h`:

```cpp
#pragma once

#include <QObject>
#include <QRemoteObjectReplica>
#include <QString>
#include <QStringList>

#include <optional>

class SerialPortActions;

namespace fastecu::desktop::connection
{

// The toolbar's log-transport combo text, parsed once.
enum class LogTransport
{
    Can,
    Iso15765,
    KLine,
    Ssm,
    Other,
};

LogTransport log_transport_from_text(const QString& text);

// MainWindow's view of the adapter: the port list, opening and resetting,
// the log-transport flag profile, the two idle resets, and battery voltage.
// Each member reproduces the facade call sequence MainWindow made before
// step 6h. Non-owning: the facade must outlive this object.
class AdapterConnection final : public QObject
{
    Q_OBJECT

  public:
    explicit AdapterConnection(SerialPortActions& facade, QObject *parent = nullptr);

    QStringList available_ports();
    void set_initial_port(const QString& port, const QString& baud);
    void select_port(const QString& port);
    // The opened port's name; empty when nothing opened.
    QString open();
    bool is_open();
    void reset();
    void apply_log_transport(LogTransport transport, bool ssm_protocol);
    // check_serial_ports' reset: every link flag cleared, 4800 baud. Parity
    // is left alone.
    void clear_link_flags();
    // disconnect_from_ecu's reset: 4800 baud, no parity. The link flags are
    // left alone, because connect_to_ecu does not reapply them.
    void return_to_idle();
    void set_port_speed(int baud);
    // Empty unless the adapter is an OpenPort, the only one that reports it.
    std::optional<unsigned long> battery_millivolts();
    void wait_for_source();
    // For the handoffs that still take the facade: the flash controller, the
    // diagnostic link, and reset_serial_to_idle.
    SerialPortActions& facade();

  signals:
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);

  private:
    SerialPortActions& facade_;
};

} // namespace fastecu::desktop::connection
```

- [ ] **Step 3: Write the package BUILD file**

Create `src/platform/desktop/common/connection/BUILD.bazel`:

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "fastecu_qttest", "qt_cc_library")

package(default_visibility = [
    "//apps/desktop:__pkg__",
    "//src/platform:__subpackages__",
    "//src/ui/desktop:__pkg__",
    "//tests:__pkg__",
])

# MainWindow's connection handling over the serial facade. Reaches
# serial_port_actions.h through serial_platform_api, as the diagnostics
# package does, so the frozen serial_qt_compat allowlist does not grow. The
# header forward-declares SerialPortActions, so the UI never sees the facade
# header.
qt_cc_library(
    name = "adapter_connection",
    srcs = ["adapter_connection.cpp"],
    hdrs = ["adapter_connection.h"],
    copts = COMMON_COPTS,
    deps = QT_DEPS + [
        "//src/platform/desktop/common/serial:serial_platform_api",
    ],
)

fastecu_qttest(
    name = "adapter_connection_test",
    src = "adapter_connection_test.cpp",
    deps = [
        ":adapter_connection",
        "//src/platform/desktop/common/serial/testing:fake_serial_backend",
        "//src/platform/desktop/common/transport:fake_backed_serial",
    ],
)
```

- [ ] **Step 4: Write the failing tests**

Create `src/platform/desktop/common/connection/adapter_connection_test.cpp`:

```cpp
#include "src/platform/desktop/common/connection/adapter_connection.h"

#include <QCoreApplication>
#include <QSerialPort>
#include <QSignalSpy>
#include <QTest>

#include <cstdint>
#include <optional>

#include <gmock/gmock.h>

#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/transport/fake_backed_serial.h"

using fastecu::desktop::connection::AdapterConnection;
using fastecu::desktop::connection::log_transport_from_text;
using fastecu::desktop::connection::LogTransport;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;

class TestAdapterConnection : public QObject
{
    Q_OBJECT

  private slots:

    void parsesTheToolbarTransportText()
    {
        QCOMPARE(log_transport_from_text("CAN"), LogTransport::Can);
        QCOMPARE(log_transport_from_text("iso15765"), LogTransport::Iso15765);
        QCOMPARE(log_transport_from_text("K-Line"), LogTransport::KLine);
        QCOMPARE(log_transport_from_text("SSM"), LogTransport::Ssm);
        QCOMPARE(log_transport_from_text(""), LogTransport::Other);
    }

    void listsPortsFromTheFacade()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), check_serial_ports()).WillOnce(Return(QStringList{"ttyUSB0 - FT232"}));
        AdapterConnection connection(*serial);
        QCOMPARE(connection.available_ports(), QStringList{"ttyUSB0 - FT232"});
    }

    void setInitialPortSetsTheBaudThenThePort()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("4800"))).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_serial_port(QString("/dev/ttyUSB0"))).WillOnce(Return(true));
        }
        AdapterConnection connection(*serial);
        connection.set_initial_port("/dev/ttyUSB0", "4800");
    }

    void selectPortReplacesThePortList()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), set_serial_port_list(QStringList{"ttyUSB0"})).WillOnce(Return(true));
        AdapterConnection connection(*serial);
        connection.select_port("ttyUSB0");
    }

    void openAndIsOpenAskTheFacade()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("ttyUSB0")));
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(Return(false));
        AdapterConnection connection(*serial);
        QCOMPARE(connection.open(), QString("ttyUSB0"));
        QVERIFY(!connection.is_open());
    }

    void canTransportIsRawCanElevenBit()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_can_connection(true)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_can_speed(QString("500000"))).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), reset_connection());
        }
        AdapterConnection connection(*serial);
        connection.apply_log_transport(LogTransport::Can, false);
    }

    // Pinned: log_transport_changed set 29-bit identifiers for iso15765.
    void iso15765TransportIsTwentyNineBit()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_iso15765_connection(true)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_29_bit_id(true)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_can_speed(QString("500000"))).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), reset_connection());
        }
        AdapterConnection connection(*serial);
        connection.apply_log_transport(LogTransport::Iso15765, false);
    }

    void klineWithSsmRunsAtFourThousandEightHundred()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), change_port_speed(QString("4800"))).WillOnce(Return(0));
            EXPECT_CALL(serial.fake(), reset_connection());
        }
        AdapterConnection connection(*serial);
        connection.apply_log_transport(LogTransport::KLine, true);
    }

    void klineWithoutSsmLeavesTheSpeedAlone()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), change_port_speed(_)).Times(0);
        EXPECT_CALL(serial.fake(), reset_connection());
        AdapterConnection connection(*serial);
        connection.apply_log_transport(LogTransport::KLine, false);
    }

    void clearLinkFlagsClearsEveryFlagAndKeepsParity()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), reset_connection());
            EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("4800"))).WillOnce(Return(true));
        }
        EXPECT_CALL(serial.fake(), set_serial_port_parity(_)).Times(0);
        AdapterConnection connection(*serial);
        connection.clear_link_flags();
    }

    void returnToIdleResetsBaudAndParityAndKeepsTheFlags()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), reset_connection());
            EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("4800"))).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)))
                .WillOnce(Return(true));
        }
        EXPECT_CALL(serial.fake(), set_is_can_connection(_)).Times(0);
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(_)).Times(0);
        AdapterConnection connection(*serial);
        connection.return_to_idle();
    }

    void setPortSpeedPassesTheBaudAsText()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), change_port_speed(QString("4800"))).WillOnce(Return(0));
        AdapterConnection connection(*serial);
        connection.set_port_speed(4800);
    }

    void batteryIsReadOnlyFromAnOpenPort()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillOnce(Return(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), read_vbatt()).WillOnce(Return(12500UL));
        AdapterConnection connection(*serial);
        QVERIFY(!connection.battery_millivolts().has_value());
        const std::optional<unsigned long> reading = connection.battery_millivolts();
        QVERIFY(reading.has_value());
        QCOMPARE(*reading, 12500UL);
    }

    void waitForSourceWaitsOnTheFacade()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), waitForSource());
        AdapterConnection connection(*serial);
        connection.wait_for_source();
    }

    void forwardsFacadeStateChanges()
    {
        FakeBackedSerial serial;
        AdapterConnection connection(*serial);
        QSignalSpy spy(&connection, &AdapterConnection::stateChanged);
        emit serial->stateChanged(QRemoteObjectReplica::Valid, QRemoteObjectReplica::Default);
        QCOMPARE(spy.count(), 1);
    }

    void exposesTheSameFacade()
    {
        FakeBackedSerial serial;
        AdapterConnection connection(*serial);
        QCOMPARE(&connection.facade(), serial.get());
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    TestAdapterConnection test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "adapter_connection_test.moc"
```

- [ ] **Step 5: Run them to verify they fail**

Run: `bazel test --config=release //src/platform/desktop/common/connection:adapter_connection_test`
Expected: FAIL to build: `adapter_connection.cpp` does not exist.

- [ ] **Step 6: Implement the adapter**

Create `src/platform/desktop/common/connection/adapter_connection.cpp`:

```cpp
#include "src/platform/desktop/common/connection/adapter_connection.h"

#include <QSerialPort>

#include <cstdint>

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::desktop::connection
{

LogTransport log_transport_from_text(const QString& text)
{
    if (text == "CAN")
    {
        return LogTransport::Can;
    }
    if (text == "iso15765")
    {
        return LogTransport::Iso15765;
    }
    if (text == "K-Line")
    {
        return LogTransport::KLine;
    }
    if (text == "SSM")
    {
        return LogTransport::Ssm;
    }
    return LogTransport::Other;
}

AdapterConnection::AdapterConnection(SerialPortActions& facade, QObject *parent) : QObject(parent), facade_(facade)
{
    connect(&facade_, &SerialPortActions::stateChanged, this, &AdapterConnection::stateChanged, Qt::DirectConnection);
}

QStringList AdapterConnection::available_ports()
{
    return facade_.check_serial_ports();
}

void AdapterConnection::set_initial_port(const QString& port, const QString& baud)
{
    facade_.set_serial_port_baudrate(baud);
    facade_.set_serial_port(port);
}

void AdapterConnection::select_port(const QString& port)
{
    facade_.set_serial_port_list(QStringList{port});
}

QString AdapterConnection::open()
{
    return facade_.open_serial_port();
}

bool AdapterConnection::is_open()
{
    return facade_.is_serial_port_open();
}

void AdapterConnection::reset()
{
    facade_.reset_connection();
}

void AdapterConnection::apply_log_transport(LogTransport transport, bool ssm_protocol)
{
    facade_.set_is_can_connection(false);
    facade_.set_is_iso15765_connection(false);
    switch (transport)
    {
    case LogTransport::Can:
        facade_.set_is_can_connection(true);
        facade_.set_is_iso15765_connection(false);
        facade_.set_is_29_bit_id(false);
        facade_.set_can_speed("500000");
        break;
    case LogTransport::Iso15765:
        facade_.set_is_can_connection(false);
        facade_.set_is_iso15765_connection(true);
        facade_.set_is_29_bit_id(true);
        facade_.set_can_speed("500000");
        break;
    case LogTransport::KLine:
        if (ssm_protocol)
        {
            facade_.change_port_speed("4800");
        }
        break;
    case LogTransport::Ssm:
    case LogTransport::Other:
        break;
    }
    facade_.reset_connection();
}

void AdapterConnection::clear_link_flags()
{
    facade_.reset_connection();
    facade_.set_is_iso14230_connection(false);
    facade_.set_is_29_bit_id(false);
    facade_.set_add_iso14230_header(false);
    facade_.set_is_can_connection(false);
    facade_.set_is_iso15765_connection(false);
    facade_.set_serial_port_baudrate("4800");
}

void AdapterConnection::return_to_idle()
{
    facade_.reset_connection();
    facade_.set_serial_port_baudrate("4800");
    facade_.set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity));
}

void AdapterConnection::set_port_speed(int baud)
{
    facade_.change_port_speed(QString::number(baud));
}

std::optional<unsigned long> AdapterConnection::battery_millivolts()
{
    if (!facade_.get_use_openport2_adapter())
    {
        return std::nullopt;
    }
    return facade_.read_vbatt();
}

void AdapterConnection::wait_for_source()
{
    facade_.waitForSource();
}

SerialPortActions& AdapterConnection::facade()
{
    return facade_;
}

} // namespace fastecu::desktop::connection
```

- [ ] **Step 7: Run the tests**

Run: `bazel test --config=release //src/platform/desktop/common/connection:adapter_connection_test //:serial_compat_allowlist`
Expected: PASS. The allowlist check prints `OK: 4 entries, none added.`

- [ ] **Step 8: Commit**

```bash
git add src/platform/desktop/common/connection
git commit -F - <<'EOF'
feat(platform): add AdapterConnection over the serial facade (step 6h-3)

A concrete Qt adapter for MainWindow's connection handling. Each member
reproduces the facade call sequence MainWindow made before this step;
tests pin the order against the fake backend. It reaches the facade
through serial_platform_api, so the frozen allowlist does not grow.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 8: Hand `MainWindow` the adapter instead of the facade

**Files:**
- Create: `src/platform/desktop/common/connection/testing/BUILD.bazel`, `adapter_connection_harness.h`, `adapter_connection_harness.cpp`
- Modify: `src/ui/desktop/main_window_services.h`, `src/ui/desktop/mainwindow.h`, `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/BUILD.bazel`
- Modify: `apps/desktop/desktop_composition.h`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/desktop_composition_test.cpp`, `apps/desktop/BUILD.bazel`
- Test: `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- Consumes: `AdapterConnection` (Task 7).
- Produces:
  - `MainWindowServices::connection` of type `fastecu::desktop::connection::AdapterConnection&`, replacing `MainWindowServices::serial`.
  - `MainWindow::connection`, of type `fastecu::desktop::connection::AdapterConnection *`. `MainWindow::serial` survives this task as `&connection->facade()` and is removed in Task 11.
  - `fastecu::desktop::connection::testing::AdapterConnectionHarness` with `FakeBackend *fake() const` and `AdapterConnection& connection()`.

- [ ] **Step 1: Write the harness**

Create `src/platform/desktop/common/connection/testing/adapter_connection_harness.h`:

```cpp
#pragma once

#include <memory>

#include "src/platform/desktop/common/connection/adapter_connection.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

class SerialPortActions;

namespace fastecu::desktop::connection::testing
{

// A facade over a NiceFakeBackend with an AdapterConnection on top. UI tests
// drive MainWindow through it without including serial_port_actions.h, which
// the UI package can no longer see.
class AdapterConnectionHarness
{
  public:
    AdapterConnectionHarness();
    ~AdapterConnectionHarness();

    AdapterConnectionHarness(const AdapterConnectionHarness&) = delete;
    AdapterConnectionHarness& operator=(const AdapterConnectionHarness&) = delete;

    // Null if the fake backend failed to start.
    FakeBackend *fake() const
    {
        return fake_;
    }

    AdapterConnection& connection()
    {
        return *connection_;
    }

  private:
    FakeBackend *fake_ = nullptr;
    std::unique_ptr<SerialPortActions> serial_;
    std::unique_ptr<AdapterConnection> connection_;
};

} // namespace fastecu::desktop::connection::testing
```

Create `src/platform/desktop/common/connection/testing/adapter_connection_harness.cpp`:

```cpp
#include "src/platform/desktop/common/connection/testing/adapter_connection_harness.h"

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::desktop::connection::testing
{

AdapterConnectionHarness::AdapterConnectionHarness()
    : serial_(std::make_unique<SerialPortActions>(
          [this]() -> SerialBackend *
          {
              auto *fake = new NiceFakeBackend;
              fake_ = fake;
              return fake;
          }))
{
    // The facade creates its backend on the first marshaled call; this one
    // is otherwise inert.
    if (!serial_->set_add_ssm_header(false))
    {
        fake_ = nullptr;
    }
    connection_ = std::make_unique<AdapterConnection>(*serial_);
}

AdapterConnectionHarness::~AdapterConnectionHarness()
{
    connection_.reset();
    serial_.reset();
}

} // namespace fastecu::desktop::connection::testing
```

Create `src/platform/desktop/common/connection/testing/BUILD.bazel`:

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "qt_cc_library")

package(default_visibility = ["//src/ui/desktop:__pkg__"])

# Lets UI-layer tests build MainWindow over a fake backend without seeing
# serial_port_actions.h: the facade lives behind the harness's .cpp.
qt_cc_library(
    name = "adapter_connection_harness",
    testonly = True,
    srcs = ["adapter_connection_harness.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["adapter_connection_harness.h"],
    deps = QT_DEPS + [
        "//src/platform/desktop/common/connection:adapter_connection",
        "//src/platform/desktop/common/serial:serial_platform_api",
        "//src/platform/desktop/common/serial/testing:fake_serial_backend",
    ],
)
```

- [ ] **Step 2: Swap the service**

Replace `src/ui/desktop/main_window_services.h` with:

```cpp
#pragma once

class FileActions;
class QtEventSink;
class QtFileRepository;
class RemoteUtility;
class SystemLogger;
namespace fastecu::desktop::connection
{
class AdapterConnection;
}
namespace fastecu::desktop::logging
{
class LoggingEngine;
}

// Long-lived services MainWindow uses but does not own. A composition root
// (apps/desktop's DesktopComposition, or a test fixture) builds them, keeps
// them alive for MainWindow's whole lifetime, and passes this struct to its
// constructor.
struct MainWindowServices
{
    FileActions& file_actions; // set_base_dirs already applied
    QtFileRepository& config_repository;
    QtEventSink& file_action_events;
    SystemLogger& syslogger;
    fastecu::desktop::connection::AdapterConnection& connection;
    RemoteUtility& remote_utility;
    fastecu::desktop::logging::LoggingEngine& logging_engine;
};
```

In `src/ui/desktop/mainwindow.h`, add `#include "src/platform/desktop/common/connection/adapter_connection.h"` after the `#include "src/platform/desktop/common/ports/qt_file_repository.h"` line, and directly above `SerialPortActions *serial = nullptr;` add:

```cpp
    fastecu::desktop::connection::AdapterConnection *connection = nullptr;
```

In `src/ui/desktop/mainwindow.cpp`, replace

```cpp
    serial = &services_.serial;
```

with

```cpp
    connection = &services_.connection;
    serial = &connection->facade();
```

In `src/ui/desktop/BUILD.bazel`, add `"//src/platform/desktop/common/connection:adapter_connection",` to the `:desktop` target's `deps`, directly above `"//src/platform/desktop/common/diagnostics:serial_diagnostic_link",`.

- [ ] **Step 3: Build and own the adapter in the composition root**

In `apps/desktop/desktop_composition.h`, add `#include "src/platform/desktop/common/connection/adapter_connection.h"` after the `desktop_serial_factory.h` include, and directly below `OwnedSerialPortActions serial_;` add:

```cpp
    std::unique_ptr<fastecu::desktop::connection::AdapterConnection> connection_;
```

In `apps/desktop/desktop_composition.cpp`, directly after the `serial_ = make_serial_port_actions(...)` line add:

```cpp
    connection_ = std::make_unique<fastecu::desktop::connection::AdapterConnection>(*serial_);
```

In the destructor, add `connection_.reset();` directly above `serial_.reset();`. In `services()`, replace `.serial = *serial_,` with `.connection = *connection_,`.

In `apps/desktop/desktop_composition_test.cpp`, replace `QCOMPARE(&first.serial, &second.serial);` with `QCOMPARE(&first.connection, &second.connection);`.

In `apps/desktop/BUILD.bazel`, add `"//src/platform/desktop/common/connection:adapter_connection",` to `:composition`'s `deps`, directly above `"//src/platform/desktop/common/logging",`.

- [ ] **Step 4: Move `mainwindow_test` onto the harness**

In `src/ui/desktop/mainwindow_test.cpp`:
- delete `#include "src/platform/desktop/common/serial/serial_port_actions.h"`;
- add `#include "src/platform/desktop/common/connection/testing/adapter_connection_harness.h"` after `#include "src/platform/desktop/common/serial/testing/fake_backend.h"`;
- delete the whole `fakeSerial` function;
- in `TestServices`, delete the constructor line `serial = fakeSerial(nullptr, &fake);` and replace the two members

```cpp
    FakeBackend *fake = nullptr;
    std::unique_ptr<SerialPortActions> serial; // null if the fake backend failed to start
```

with

```cpp
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake(); // null if the fake backend failed to start
```

  The `adapter` member must stay declared above `fake`, so that it is initialised first.
- in `TestServices::services()`, replace `.serial = *serial,` with `.connection = adapter.connection(),`;
- replace every `QVERIFY(services.serial != nullptr);` with `QVERIFY(services.fake != nullptr);`.

In `src/ui/desktop/BUILD.bazel`, add `"//src/platform/desktop/common/connection/testing:adapter_connection_harness",` to `test_mainwindow`'s `deps`.

- [ ] **Step 5: Run the tests**

Run:

```bash
bazel test --config=release //src/ui/desktop:test_mainwindow //apps/desktop:desktop_composition_test //src/platform/desktop/common/connection:all
grep -n "serial_port_actions.h" src/ui/desktop/mainwindow_test.cpp
```

Expected: every test passes. The `grep` prints nothing.

- [ ] **Step 6: Commit**

```bash
git add src/platform/desktop/common/connection/testing src/ui/desktop apps/desktop
git commit -F - <<'EOF'
refactor(desktop): hand MainWindow an AdapterConnection (step 6h-3)

MainWindowServices carries the adapter instead of the facade, and
DesktopComposition builds and owns it. MainWindow still reaches the
facade through connection->facade() until its calls move over.
mainwindow_test builds its services through a platform harness and no
longer includes serial_port_actions.h.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 9: Route `MainWindow`'s connection calls through the adapter

**Files:**
- Modify: `src/ui/desktop/mainwindow.h`, `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/menu_actions.cpp`
- Test: `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- Consumes: `AdapterConnection` (Task 7); `MainWindow::connection` (Task 8).
- Produces: `QString MainWindow::selected_serial_port() const`, defined inline in the header, which returns the empty string when there is no port. After this task, the only `serial->` calls left in the `:desktop` sources are in `log_operations_ssm.cpp`.

- [ ] **Step 1: Write the failing tests**

Add these slots to `MainWindowTest` in `src/ui/desktop/mainwindow_test.cpp`, before `private:`:

```cpp
    void selectedSerialPortIsEmptyWithoutPorts()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        window.serial_ports.clear();
        window.serial_port_list->clear();
        QCOMPARE(window.selected_serial_port(), QString());
        window.serial_ports = {"ttyUSB0"};
        window.serial_port_list->addItem("ttyUSB0");
        QCOMPARE(window.selected_serial_port(), QString("ttyUSB0"));
    }

    void dtcWindowWithoutAPortWarnsInsteadOfCrashing()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        window.serial_ports.clear();
        window.serial_port_list->clear();
        EXPECT_CALL(*services.fake, set_serial_port_list(::testing::_)).Times(0);
        ModalDriver driver{QString()};
        driver.start();
        for (const char *command : {"dtc_window", "biu_communication", "terminal"})
        {
            QVERIFY(QMetaObject::invokeMethod(&window, "menu_action_triggered", Qt::DirectConnection,
                                              Q_ARG(QString, QString::fromLatin1(command))));
        }
        driver.stop();
        QVERIFY(!driver.timedOut());
    }

    void disconnectReturnsTheAdapterToIdle()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        {
            ::testing::InSequence order;
            EXPECT_CALL(*services.fake, reset_connection());
            EXPECT_CALL(*services.fake, set_serial_port_baudrate(QString("4800")));
            EXPECT_CALL(*services.fake, set_serial_port_parity(0));
        }
        EXPECT_CALL(*services.fake, set_is_can_connection(::testing::_)).Times(0);
        QVERIFY(QMetaObject::invokeMethod(&window, "menu_action_triggered", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("disconnect_from_ecu"))));
        // Check now, so facade teardown cannot over-saturate the expectations.
        QVERIFY(::testing::Mock::VerifyAndClearExpectations(services.fake));
        QVERIFY(window.serial_port_list->isEnabled());
    }
```

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow`
Expected: FAIL to compile, because `MainWindow::selected_serial_port` does not exist. Once it exists, `dtcWindowWithoutAPortWarnsInsteadOfCrashing` would still crash on `serial_ports.at(...)`.

- [ ] **Step 3: Add the helper**

In `src/ui/desktop/mainwindow.h`, directly below `QStringList serial_ports;`, add:

```cpp
    // The port chosen in the toolbar, or empty when there is none. Inline so
    // tests reaching it through `#define private public` link on MSVC too.
    QString selected_serial_port() const
    {
        return serial_ports.value(serial_port_list->currentIndex());
    }
```

- [ ] **Step 4: Route the constructor, port polling, transport, and vbatt**

In `src/ui/desktop/mainwindow.cpp`:

1. In the constructor, replace `serial->waitForSource();` with `connection->wait_for_source();`. Replace

```cpp
    connect(serial, &SerialPortActions::stateChanged, this, &MainWindow::network_state_changed, Qt::DirectConnection);
```

   with

```cpp
    connect(connection, &fastecu::desktop::connection::AdapterConnection::stateChanged, this,
            &MainWindow::network_state_changed, Qt::DirectConnection);
```

   Replace `serial_ports = serial->check_serial_ports();` with `serial_ports = connection->available_ports();`. Replace the two lines

```cpp
    serial->set_serial_port_baudrate(serial_port_baudrate);
    serial->set_serial_port(serial_port);
```

   with `connection->set_initial_port(serial_port, serial_port_baudrate);`.

2. Replace the body of `MainWindow::log_transport_changed()` with:

```cpp
    QComboBox *log_transport_list = ui->toolBar->findChild<QComboBox *>("log_transport_list");

    connection->apply_log_transport(
        fastecu::desktop::connection::log_transport_from_text(log_transport_list->currentText()),
        configValues->flash_protocol_selected_log_protocol == "SSM");

    protocol = configValues->flash_protocol_selected_log_protocol;
    configValues->flash_protocol_selected_log_transport = log_transport_list->currentText();
    fileActions->save_config_file(configValues);

    ecuid.clear();
    ecu_init_complete = false;
```

3. Replace the body of `MainWindow::check_serial_ports()` with:

```cpp
    QComboBox *serial_port_list = ui->toolBar->findChild<QComboBox *>("serial_port_list");
    QString prev_serial_port = serial_port_list->currentText();
    int index = 0;

    connection->clear_link_flags();
    ecuid.clear();
    ecu_init_complete = false;
    emit log_transport_list->currentIndexChanged(log_transport_list->currentIndex());

    serial_ports = connection->available_ports();
    serial_port_list->clear();

    for (int i = 0; i < serial_ports.length(); i++)
    {
        serial_port_list->addItem(serial_ports.at(i));
        if (prev_serial_port == serial_ports.at(i))
        {
            serial_port_list->setCurrentIndex(index);
        }
        index++;
    }
```

4. Replace the body of `MainWindow::open_serial_port()` with:

```cpp
    const QString port = selected_serial_port();
    if (port.isEmpty())
    {
        return;
    }
    connection->select_port(port);
    QString opened_serial_port = connection->open();
    if (opened_serial_port != "")
    {
        if (opened_serial_port != previous_serial_port)
        {
            ecuid.clear();
            ecu_init_complete = false;
        }
        previous_serial_port = opened_serial_port;
        configValues->serial_port = port;
        fileActions->save_config_file(configValues);
        if (ecuid == "")
        {
            set_status_bar_label(true, false, "");
        }
        else
        {
            set_status_bar_label(true, true, ecuid);
        }
    }
    else
    {
        set_status_bar_label(false, false, "");
        ecu_init_complete = false;
    }
```

5. In `MainWindow::update_vbatt()`, replace

```cpp
    unsigned long vBatt = 0;

    if (!serial->get_use_openport2_adapter())
    {
        return;
    }

    vBatt = serial->read_vbatt();
```

   with

```cpp
    const std::optional<unsigned long> reading = connection->battery_millivolts();
    if (!reading.has_value())
    {
        return;
    }
    const unsigned long vBatt = *reading;
```

6. In `MainWindow::start_ecu_operations`, replace

```cpp
    QStringList spl;
    spl.append(serial_ports.at(serial_port_list->currentIndex()));

    serial->set_serial_port_list(spl);
```

   with `connection->select_port(selected_serial_port());`. Replace both `fastecu::desktop::serial::reset_serial_to_idle(*serial);` with `fastecu::desktop::serial::reset_serial_to_idle(connection->facade());`. Replace `serial->change_port_speed("4800");` with `connection->set_port_speed(4800);`, and `fastecu::flash::FlashOperationController controller{*serial, this};` with `fastecu::flash::FlashOperationController controller{connection->facade(), this};`.

- [ ] **Step 5: Route connect, disconnect, and the dialogs**

In `src/ui/desktop/menu_actions.cpp`:

1. In `MainWindow::connect_to_ecu()`, replace `serial->reset_connection();` with `connection->reset();`, and `if (serial->is_serial_port_open())` with `if (connection->is_open())`.

2. Replace the body of `MainWindow::disconnect_from_ecu()` with:

```cpp
    qDebug() << "Disconnecting...";
    ecuid.clear();
    ecu_init_complete = false;
    set_status_bar_label(false, false, "");
    connection->return_to_idle();

    serial_port_list->setEnabled(true);
    refresh_serial_port_list->setEnabled(true);
```

3. In `MainWindow::show_dtc_window()`, replace the lines from `serial->reset_connection();` through `serial->set_serial_port_list(spl);` with:

```cpp
    const QString port = selected_serial_port();
    if (port.isEmpty())
    {
        QMessageBox::warning(this, tr("Serial port"), "No serial port selected!");
        return;
    }
    connection->reset();
    ecuid.clear();
    ecu_init_complete = false;
    connection->select_port(port);
```

   and replace `fastecu::diagnostics::SerialDiagnosticLink link(serial);` with `fastecu::diagnostics::SerialDiagnosticLink link(&connection->facade());`.

4. Replace the body of `MainWindow::show_subaru_biu_window()` with:

```cpp
    const QString port = selected_serial_port();
    if (port.isEmpty())
    {
        QMessageBox::warning(this, tr("Serial port"), "No serial port selected!");
        return;
    }
    ecuid.clear();
    ecu_init_complete = false;
    connection->select_port(port);

    fastecu::diagnostics::SerialDiagnosticLink link(&connection->facade());
    const auto opened = link.open(fastecu::diagnostics::KlineLinkConfig{
        .header = fastecu::diagnostics::KlineHeader::None, .iso14230_connection = true, .baud = 10400});
    set_status_bar_label(opened.has_value(), false, "");
    if (!opened.has_value())
    {
        emit LOG_E("BIU: could not open the interface: " + QString::fromStdString(opened.error().detail), true, true);
    }

    BiuOperationsSubaru biuOperationsSubaru(link, this);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_D, syslogger, &SystemLogger::log_messages);

    biuOperationsSubaru.exec();

    emit LOG_D("BIU stopped", true, true);

    static_cast<void>(link.set_header(fastecu::diagnostics::KlineHeader::None));
```

5. In `MainWindow::show_terminal_window()`, replace the three lines from `QStringList serial_port_arg;` through `serial->set_serial_port_list(serial_port_arg);` with:

```cpp
    const QString port = selected_serial_port();
    if (port.isEmpty())
    {
        QMessageBox::warning(this, tr("Serial port"), "No serial port selected!");
        return;
    }
    connection->select_port(port);
```

   and replace `fastecu::diagnostics::SerialDiagnosticLink link(serial);` with `fastecu::diagnostics::SerialDiagnosticLink link(&connection->facade());`.

- [ ] **Step 6: Confirm only the SSM init code still calls the facade**

Run: `grep -n "serial->" src/ui/desktop/mainwindow.cpp src/ui/desktop/menu_actions.cpp src/ui/desktop/log_operations_ssm.cpp`
Expected: matches only in `log_operations_ssm.cpp`, plus commented-out lines (`//`) in `mainwindow.cpp`.

- [ ] **Step 7: Run the PR's tests and gates**

Run:

```bash
bazel test --config=release //src/ui/desktop:all //apps/desktop:all //src/platform/desktop/common/connection/...:all
bazel build --config=release //:fastecu
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Expected: every command passes. In particular, `handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling` still sees exactly 3 `reset_connection` calls and 2 `change_port_speed("4800")` calls. That proves `start_ecu_operations` made the same facade calls through the adapter.

- [ ] **Step 8: Commit**

```bash
git add src/ui/desktop
git commit -F - <<'EOF'
refactor(ui): route MainWindow's connection calls through the adapter (step 6h-3)

Port listing, opening, transport flags, idle resets, battery voltage,
dialog preparation, and flash-operation port selection now go through
AdapterConnection. BIU is prepared by opening the diagnostic link at
10400. With no port present, the DTC, BIU, and terminal commands warn
instead of indexing past the end of the port list. Only the SSM
identification code still calls the facade.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

---

## PR 6h-4: identification worker, async connect, and close-out

### Task 10: `SsmIdentifyWorker`

**Files:**
- Create: `src/platform/desktop/common/diagnostics/ssm_identify_worker.h`, `ssm_identify_worker.cpp`
- Test: `src/platform/desktop/common/diagnostics/ssm_identify_worker_test.cpp`
- Modify: `src/platform/desktop/common/diagnostics/BUILD.bazel`

**Interfaces:**
- Consumes: `identify_ssm_ecu`, `SsmIdentifyRequest`, `SsmIdentity` (Tasks 3–6); `IDiagnosticLink`; `IClock`; `ManualCancellationToken`; `fastecu::LogLevel` from `src/backend/ports/event_sink.h`.
- Produces, in namespace `fastecu::diagnostics`:
  - `struct SsmIdentifyWorkerResult { bool success = false; ErrorKind error_kind = ErrorKind::Internal; QString error_detail; QString ecu_id; QByteArray init_response; };`, registered with `Q_DECLARE_METATYPE`.
  - `class SsmIdentifyWorker final : public QThread` with the constructor `(SsmIdentifyRequest, IDiagnosticLink&, std::unique_ptr<IClock>, QObject *parent = nullptr)`, `requestStop()`, and the signals `logEvent(int level, QString message)` and `completed(fastecu::diagnostics::SsmIdentifyWorkerResult)`. It also has `static constexpr int kMaxAttempts = 5` and `static constexpr std::chrono::milliseconds kRetryDelay{500}`.

- [ ] **Step 1: Create the branch**

```bash
git switch -c refactor/step6h-4-connect-worker
```

- [ ] **Step 2: Write the header**

Create `src/platform/desktop/common/diagnostics/ssm_identify_worker.h`:

```cpp
#pragma once

#include <QByteArray>
#include <QString>
#include <QThread>

#include <chrono>
#include <memory>

#include "src/backend/diagnostics/ssm_identify.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/idiagnostic_link.h"

namespace fastecu::diagnostics
{

// Qt-friendly outcome: std::expected is not a Qt metatype.
struct SsmIdentifyWorkerResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::Internal;
    QString error_detail;
    QString ecu_id;
    QByteArray init_response;
};

// Runs connect_to_ecu's identification retry loop on its own thread. Shaped
// like DtcWorker. The link is not owned and must outlive the worker.
class SsmIdentifyWorker final : public QThread
{
    Q_OBJECT

  public:
    static constexpr int kMaxAttempts = 5;
    static constexpr std::chrono::milliseconds kRetryDelay{500};

    SsmIdentifyWorker(SsmIdentifyRequest request, IDiagnosticLink& link, std::unique_ptr<IClock> clock,
                      QObject *parent = nullptr);
    ~SsmIdentifyWorker() override;

    SsmIdentifyWorker(const SsmIdentifyWorker&) = delete;
    SsmIdentifyWorker& operator=(const SsmIdentifyWorker&) = delete;

    // Safe from any thread, any number of times, before or after start().
    void requestStop();

  signals:
    void logEvent(int level, QString message);
    // Emitted exactly once per run(), from the worker thread.
    void completed(fastecu::diagnostics::SsmIdentifyWorkerResult result);

  protected:
    void run() override;

  private:
    SsmIdentifyRequest request_;
    IDiagnosticLink& link_;
    std::unique_ptr<IClock> clock_;
    ManualCancellationToken cancellation_;
};

} // namespace fastecu::diagnostics

Q_DECLARE_METATYPE(fastecu::diagnostics::SsmIdentifyWorkerResult)
```

- [ ] **Step 3: Write the failing tests**

Create `src/platform/desktop/common/diagnostics/ssm_identify_worker_test.cpp`:

```cpp
#include "src/platform/desktop/common/diagnostics/ssm_identify_worker.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <memory>
#include <string>

#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::diagnostics::FakeDiagnosticLink;
using fastecu::diagnostics::SsmIdentifyRequest;
using fastecu::diagnostics::SsmIdentifyWorker;
using fastecu::diagnostics::SsmIdentifyWorkerResult;
using fastecu::diagnostics::SsmTarget;
using fastecu::diagnostics::SsmVariant;

namespace
{
const bytes::Bytes kShortEcuInit{0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C};

int opens(const FakeDiagnosticLink& link)
{
    return static_cast<int>(std::count_if(link.calls.begin(), link.calls.end(),
                                          [](const std::string& call) { return call.starts_with("open kline"); }));
}
} // namespace

class SsmIdentifyWorkerTest : public QObject
{
    Q_OBJECT

  private slots:

    void stopsAtTheFirstSuccess()
    {
        FakeDiagnosticLink link;
        link.queue_read(kShortEcuInit);
        auto clock = std::make_unique<FakeClock>();
        SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::KlineSsm2, SsmTarget::Ecu}, link, std::move(clock));
        QSignalSpy done(&worker, &SsmIdentifyWorker::completed);
        worker.start();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<SsmIdentifyWorkerResult>();
        QVERIFY(result.success);
        QCOMPARE(result.ecu_id, QString("3152584006"));
        QCOMPARE(result.init_response.size(), qsizetype{14});
        QCOMPARE(opens(link), 1);
    }

    void retriesFiveTimesThenReportsTheLastError()
    {
        FakeDiagnosticLink link;
        auto clock = std::make_unique<FakeClock>();
        FakeClock *clock_view = clock.get();
        SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::KlineSsm2, SsmTarget::Ecu}, link, std::move(clock));
        QSignalSpy logs(&worker, &SsmIdentifyWorker::logEvent);
        QSignalSpy done(&worker, &SsmIdentifyWorker::completed);
        worker.start();
        QVERIFY(done.wait(5000));
        worker.wait();
        const auto result = done.at(0).at(0).value<SsmIdentifyWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Timeout);
        QCOMPARE(opens(link), 5);
        QCOMPARE(logs.count(), 5);
        // Five 200 ms settle sleeps inside the attempts and four 500 ms gaps
        // between them; no sleep after the last attempt.
        QCOMPARE(clock_view->elapsed().count(), 3000);
    }

    void stopBeforeStartCancelsAfterOneAttempt()
    {
        FakeDiagnosticLink link;
        SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::KlineSsm2, SsmTarget::Ecu}, link,
                                 std::make_unique<FakeClock>());
        QSignalSpy done(&worker, &SsmIdentifyWorker::completed);
        worker.requestStop();
        worker.start();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.at(0).at(0).value<SsmIdentifyWorkerResult>().error_kind, ErrorKind::Cancelled);
        QCOMPARE(opens(link), 1);
    }

    void destroyingARunningWorkerJoinsIt()
    {
        FakeDiagnosticLink link;
        {
            SsmIdentifyWorker worker(SsmIdentifyRequest{SsmVariant::KlineSsm2, SsmTarget::Ecu}, link,
                                     std::make_unique<FakeClock>());
            worker.start();
        }
        // The destructor returned, so the thread has stopped touching the link.
        const auto calls_after = link.calls.size();
        QTest::qWait(50);
        QCOMPARE(link.calls.size(), calls_after);
    }
};

QTEST_MAIN(SsmIdentifyWorkerTest)
#include "ssm_identify_worker_test.moc"
```

Append to `src/platform/desktop/common/diagnostics/BUILD.bazel`:

```python
# Runs the connect-time SSM identification retry loop on its own thread,
# shaped like DtcWorker.
qt_cc_library(
    name = "ssm_identify_worker",
    srcs = ["ssm_identify_worker.cpp"],
    hdrs = ["ssm_identify_worker.h"],
    copts = COMMON_COPTS,
    deps = QT_DEPS + [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/qt_compat",
        "//src/backend/diagnostics:ssm_identify",
        "//src/backend/ports",
        "//src/backend/protocol",
    ],
)

fastecu_qttest(
    name = "ssm_identify_worker_test",
    src = "ssm_identify_worker_test.cpp",
    deps = [
        ":ssm_identify_worker",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/protocol/testing:fake_diagnostic_link",
    ],
)
```

- [ ] **Step 4: Run them to verify they fail**

Run: `bazel test --config=release //src/platform/desktop/common/diagnostics:ssm_identify_worker_test`
Expected: FAIL to build: `ssm_identify_worker.cpp` does not exist.

- [ ] **Step 5: Implement the worker**

Create `src/platform/desktop/common/diagnostics/ssm_identify_worker.cpp`:

```cpp
#include "src/platform/desktop/common/diagnostics/ssm_identify_worker.h"

#include <utility>

#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/ports/event_sink.h"

namespace fastecu::diagnostics
{

SsmIdentifyWorker::SsmIdentifyWorker(SsmIdentifyRequest request, IDiagnosticLink& link, std::unique_ptr<IClock> clock,
                                     QObject *parent)
    : QThread(parent), request_(request), link_(link), clock_(std::move(clock))
{
    qRegisterMetaType<SsmIdentifyWorkerResult>();
}

SsmIdentifyWorker::~SsmIdentifyWorker()
{
    requestStop();
    // run() uses owned members; join fully before they are destroyed.
    wait();
}

void SsmIdentifyWorker::requestStop()
{
    cancellation_.cancel();
}

void SsmIdentifyWorker::run()
{
    Result<SsmIdentity> outcome = fail(ErrorKind::Internal, "no identification attempt ran");
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt)
    {
        if (attempt > 1)
        {
            if (auto slept = clock_->sleep(kRetryDelay, cancellation_); !slept.has_value())
            {
                outcome = std::unexpected(slept.error());
                break;
            }
        }
        outcome = identify_ssm_ecu(link_, *clock_, cancellation_, request_);
        if (outcome.has_value() || outcome.error().kind == ErrorKind::Cancelled)
        {
            break;
        }
        emit logEvent(static_cast<int>(LogLevel::Warning),
                      QString("ECU identification attempt %1 of %2 failed: %3")
                          .arg(attempt)
                          .arg(kMaxAttempts)
                          .arg(QString::fromStdString(outcome.error().detail)));
    }

    SsmIdentifyWorkerResult result;
    result.success = outcome.has_value();
    if (outcome.has_value())
    {
        result.ecu_id = QString::fromStdString(outcome->ecu_id);
        result.init_response = bytes::toQByteArray(outcome->init_response);
    }
    else
    {
        result.error_kind = outcome.error().kind;
        result.error_detail = QString::fromStdString(outcome.error().detail);
    }
    emit completed(result);
}

} // namespace fastecu::diagnostics
```

- [ ] **Step 6: Run the tests**

Run: `bazel test --config=release //src/platform/desktop/common/diagnostics:all`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/platform/desktop/common/diagnostics
git commit -F - <<'EOF'
feat(diagnostics): run SSM identification on a worker thread (step 6h-4)

SsmIdentifyWorker runs connect_to_ecu's five-attempt retry loop off the
UI thread, shaped like DtcWorker: cancellation ends it at once, each
failed attempt is logged, and completed() is emitted exactly once.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 11: Asynchronous connect, the logging continuation, and the legacy SSM code deleted

**Files:**
- Modify: `src/ui/desktop/mainwindow.h`, `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/menu_actions.cpp`, `src/ui/desktop/log_operations_ssm.cpp`, `src/ui/desktop/BUILD.bazel`
- Test: `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- Consumes: `SsmIdentifyWorker`, `SsmIdentifyWorkerResult` (Task 10); `SsmVariant`, `SsmTarget`, `SsmIdentifyRequest` (Task 3); `SerialDiagnosticLink`; `QtClock`; `AdapterConnection` (Task 7).
- Produces, as `MainWindow` members:
  - `void connect_to_ecu(std::function<void(bool)> on_done = {});`
  - `void continue_start_logging();`
  - `void finish_identification(const fastecu::diagnostics::SsmIdentifyWorkerResult& result);`
  - `void stop_identification();`
  - `void set_identification_in_progress(bool in_progress);`
  - data members `identify_link_`, `identify_worker_`, `connect_done_`, `identify_generation_`.
  - Removed: `ecu_init`, `ssm_init`, `ssm_kline_init`, `ssm_can_init`, `add_ssm_header`, `calculate_checksum`, `parse_ecuid`, `ecu_init_started`, and the `serial` member.

- [ ] **Step 1: Write the failing tests**

Add at the top of `src/ui/desktop/mainwindow_test.cpp`'s anonymous namespace, after `writeTextFile`:

```cpp
QByteArray frame(std::initializer_list<int> values)
{
    QByteArray out;
    for (int v : values)
    {
        out.append(static_cast<char>(v));
    }
    return out;
}

// A valid SSM2 ECU init response carrying ECU ID 3152584006.
const QByteArray kEcuInit = frame({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C});

// Points the window at one open port on the given make and log transport.
void prepareConnect(MainWindow& window, FakeBackend& fake, const QString& make, const QString& transport)
{
    window.vbatt_timer->stop();
    window.serial_ports = {"ttyUSB0"};
    window.serial_port_list->clear();
    window.serial_port_list->addItem("ttyUSB0");
    window.configValues->flash_protocol_selected_make = make;
    window.configValues->flash_protocol_selected_log_transport = transport;
    window.configValues->flash_protocol_selected_log_protocol = "SSM";
    ON_CALL(fake, open_serial_port()).WillByDefault(::testing::Return(QString("ttyUSB0")));
}

bool triggerMenu(MainWindow& window, const char *command)
{
    return QMetaObject::invokeMethod(&window, "menu_action_triggered", Qt::DirectConnection,
                                     Q_ARG(QString, QString::fromLatin1(command)));
}
```

Add `#include <initializer_list>` beside the other standard includes. Then add these slots to `MainWindowTest`, before `private:`:

```cpp
    void connectOnAnotherMakeDisconnectsWithoutIdentifying()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Mitsubishi", "K-Line");
        EXPECT_CALL(*services.fake, write_serial_data_echo_check(::testing::_)).Times(0);
        EXPECT_CALL(*services.fake, set_serial_port_parity(0)).Times(::testing::AtLeast(1));

        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QVERIFY(elapsed.elapsed() < 1000); // the legacy loop waited 2.5 s here
        QVERIFY(window.identify_worker_ == nullptr);
        QVERIFY(!window.ecu_init_complete);
        QVERIFY(window.serial_port_list->isEnabled());
    }

    void subaruKlineConnectIdentifiesOffTheUiThread()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");
        EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
            .WillOnce(::testing::Return(kEcuInit))
            .WillRepeatedly(::testing::Return(QByteArray{}));

        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QVERIFY(window.identify_worker_ != nullptr);
        QVERIFY(!window.log_transport_list->isEnabled());
        QVERIFY(!window.serial_port_list->isEnabled());

        QTRY_VERIFY_WITH_TIMEOUT(window.ecu_init_complete, 5000);
        QCOMPARE(window.ecuid, QString("3152584006"));
        QVERIFY(window.identify_worker_ == nullptr);
        QVERIFY(window.log_transport_list->isEnabled());
        QVERIFY(!window.serial_port_list->isEnabled()); // stays locked while connected, as before
    }

    void subaruConnectThatNeverAnswersDisconnectsAndRestoresControls()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");

        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QTRY_VERIFY_WITH_TIMEOUT(window.identify_worker_ == nullptr, 15000);
        QVERIFY(!window.ecu_init_complete);
        QVERIFY(window.log_transport_list->isEnabled());
        QVERIFY(window.serial_port_list->isEnabled());
    }

    void disconnectDuringIdentificationCancelsAndDropsTheResult()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "K-Line");

        QVERIFY(triggerMenu(window, "connect_to_ecu"));
        QVERIFY(window.identify_worker_ != nullptr);
        QVERIFY(triggerMenu(window, "disconnect_from_ecu"));
        QVERIFY(window.identify_worker_ == nullptr);
        QVERIFY(window.log_transport_list->isEnabled());
        QVERIFY(window.serial_port_list->isEnabled());
        QTest::qWait(200); // any completion already queued must be dropped
        QVERIFY(!window.ecu_init_complete);
    }

    void loggingStartWaitsForIdentification()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.fake != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();
        prepareConnect(window, *services.fake, "Subaru", "iso15765");
        window.protocol = "SSM";
        EXPECT_CALL(*services.fake, read_serial_data(::testing::_))
            .WillOnce(::testing::Return(frame({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x12, 0x34, 0x56, 0x78, 0x9A})))
            .WillRepeatedly(::testing::Return(QByteArray{}));
        auto *menu = window.ui->menubar->addMenu("Test logging");
        auto *action = menu->addAction("Logging");
        action->setCheckable(true);
        auto& values = *window.logValues;
        values = FileActions::LogValuesStructure{};
        values.log_value_id = {"rpm"};
        values.log_value_protocol = {"SSM"};
        values.log_value_name = {"rpm"};
        values.log_value_description = {"rpm"};
        values.log_value_ecu_byte_index = {"0"};
        values.log_value_ecu_bit = {"0"};
        values.log_value_target = {"ECU"};
        values.log_value_address = {"000010"};
        values.log_value_conversions = {{{"rpm", "x", "0", "0", "100", "1"}}};
        values.log_value_length = {"1"};
        values.log_value = {"0"};
        values.log_value_enabled = {"1"};
        values.lower_panel_log_value_id = {"rpm"};
        services.logging_engine.registerProtocol(
            "SSM",
            [](const fastecu::desktop::logging::DesktopLoggingSnapshot&)
            {
                auto protocol = std::make_unique<ScriptedLoggingProtocol>();
                protocol->blockPollUntilCancelled();
                return protocol;
            });

        action->setChecked(true);
        QVERIFY(triggerMenu(window, "toggle_realtime"));
        QVERIFY(!window.activeLoggingSnapshot.has_value()); // still identifying
        QTRY_VERIFY_WITH_TIMEOUT(window.activeLoggingSnapshot.has_value(), 5000);
        QCOMPARE(window.ecuid, QString("123456789A"));
        services.logging_engine.stop();
    }
```

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow`
Expected: FAIL to compile: `MainWindow` has no member `identify_worker_`.

- [ ] **Step 3: Declare the new members and remove the old ones**

In `src/ui/desktop/mainwindow.h`:
- add, after the `adapter_connection.h` include:

```cpp
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"
#include "src/platform/desktop/common/diagnostics/ssm_identify_worker.h"

#include <functional>
```

- delete `bool ecu_init_started = false;`, `SerialPortActions *serial = nullptr;`, and the `// Forward declaration` comment together with its `class SerialPortActions;` line;
- delete these declarations:

```cpp
    void ssm_init();
    void ssm_kline_init();
    void ssm_can_init();
```

```cpp
    QByteArray add_ssm_header(QByteArray output, bool dec_0x100);
    uint8_t calculate_checksum(const QByteArray& output, bool dec_0x100);
```

```cpp
    bool ecu_init();
```

```cpp
    QString parse_ecuid(QByteArray received);
```

- replace `int connect_to_ecu();` with:

```cpp
    // Opens the port and, for Subaru, identifies the ECU on a worker thread.
    // on_done(false) means the port did not open or identification was
    // stopped; on_done(true) means the port opened, whether or not the ECU
    // answered (unchanged from the synchronous code).
    void connect_to_ecu(std::function<void(bool)> on_done = {});
    void continue_start_logging();
    void finish_identification(const fastecu::diagnostics::SsmIdentifyWorkerResult& result);
    // Cancels and joins a running identification, restores the controls, and
    // tells a waiting caller the connect did not complete.
    void stop_identification();
    void set_identification_in_progress(bool in_progress);

    // Declared link first, so the worker (which uses it) is destroyed first.
    std::unique_ptr<fastecu::diagnostics::SerialDiagnosticLink> identify_link_;
    std::unique_ptr<fastecu::diagnostics::SsmIdentifyWorker> identify_worker_;
    std::function<void(bool)> connect_done_;
    // Bumped by every start and stop, so a completion queued by a worker that
    // was stopped is recognised as stale and dropped.
    quint64 identify_generation_ = 0;
```

In `src/ui/desktop/BUILD.bazel`, add `"//src/backend/diagnostics:ssm_identify",` and `"//src/platform/desktop/common/diagnostics:ssm_identify_worker",` to the `:desktop` target's `deps`.

- [ ] **Step 4: Implement the connect flow**

In `src/ui/desktop/menu_actions.cpp`, add these includes after the `serial_diagnostic_link.h` include:

```cpp
#include "src/backend/diagnostics/ssm_identify.h"
#include "src/backend/ports/event_sink.h"
#include "src/platform/desktop/common/diagnostics/ssm_identify_worker.h"
#include "src/platform/desktop/common/ports/qt_clock.h"

#include <optional>
```

Add this anonymous namespace directly above `void MainWindow::menu_action_triggered`:

```cpp
namespace
{
// The log transport's identification exchange. Raw CAN never identified: its
// legacy branch tested a protocol value no configuration sets.
std::optional<fastecu::diagnostics::SsmVariant> ssm_variant_for_transport(const QString& transport)
{
    if (transport == "SSM")
    {
        return fastecu::diagnostics::SsmVariant::Ssm1;
    }
    if (transport == "K-Line")
    {
        return fastecu::diagnostics::SsmVariant::KlineSsm2;
    }
    if (transport == "iso15765")
    {
        return fastecu::diagnostics::SsmVariant::Iso15765Uds;
    }
    return std::nullopt;
}
} // namespace
```

Replace the whole of `int MainWindow::connect_to_ecu()` with:

```cpp
void MainWindow::connect_to_ecu(std::function<void(bool)> on_done)
{
    stop_identification();
    ecuid.clear();
    ecu_init_complete = false;
    set_status_bar_label(false, false, "");
    connection->reset();

    qDebug() << "Opening interface, please wait...";
    open_serial_port();
    if (!connection->is_open())
    {
        QMessageBox::warning(this, tr("Serial port"), "Could not open interface!");
        if (on_done)
        {
            on_done(false);
        }
        return;
    }
    serial_port_list->setDisabled(true);
    refresh_serial_port_list->setDisabled(true);

    const std::optional<fastecu::diagnostics::SsmVariant> variant =
        configValues->flash_protocol_selected_make == "Subaru"
            ? ssm_variant_for_transport(configValues->flash_protocol_selected_log_transport)
            : std::nullopt;
    if (!variant.has_value())
    {
        // ecu_init did nothing for other makes and for raw CAN; the legacy
        // loop spent 2.5 s on it and then disconnected.
        disconnect_from_ecu();
        if (on_done)
        {
            on_done(true);
        }
        return;
    }

    qDebug() << "Initialising ECU, please wait...";
    connect_done_ = std::move(on_done);
    set_identification_in_progress(true);
    const quint64 generation = ++identify_generation_;
    identify_link_ = std::make_unique<fastecu::diagnostics::SerialDiagnosticLink>(&connection->facade());
    identify_worker_ = std::make_unique<fastecu::diagnostics::SsmIdentifyWorker>(
        fastecu::diagnostics::SsmIdentifyRequest{*variant, ecu_radio_button->isChecked()
                                                               ? fastecu::diagnostics::SsmTarget::Ecu
                                                               : fastecu::diagnostics::SsmTarget::Tcu},
        *identify_link_, std::make_unique<QtClock>());
    connect(
        identify_worker_.get(), &fastecu::diagnostics::SsmIdentifyWorker::logEvent, this,
        [this](int level, const QString& message)
        {
            if (level == static_cast<int>(fastecu::LogLevel::Warning))
            {
                emit LOG_W(message, true, true);
            }
            else
            {
                emit LOG_D(message, true, true);
            }
        },
        Qt::QueuedConnection);
    connect(
        identify_worker_.get(), &fastecu::diagnostics::SsmIdentifyWorker::completed, this,
        [this, generation](const fastecu::diagnostics::SsmIdentifyWorkerResult& result)
        {
            if (generation == identify_generation_)
            {
                finish_identification(result);
            }
        },
        Qt::QueuedConnection);
    identify_worker_->start();
}

void MainWindow::finish_identification(const fastecu::diagnostics::SsmIdentifyWorkerResult& result)
{
    identify_worker_.reset(); // joins; run() has already returned or is returning
    identify_link_.reset();
    set_identification_in_progress(false);
    if (result.success)
    {
        ecu_init_complete = true;
        ecuid = result.ecu_id;
        emit LOG_D("ECU ID: " + ecuid, true, true);
        set_status_bar_label(true, !ecuid.isEmpty(), ecuid);
        if (!result.init_response.isEmpty())
        {
            parse_log_value_list(result.init_response, "SSM");
        }
    }
    else
    {
        emit LOG_W("ECU identification failed: " + result.error_detail, true, true);
        disconnect_from_ecu();
    }
    // A port that opened counts as connected even when identification failed.
    if (auto done = std::exchange(connect_done_, {}); done)
    {
        done(true);
    }
}

void MainWindow::stop_identification()
{
    if (!identify_worker_)
    {
        return;
    }
    ++identify_generation_;
    identify_worker_->requestStop();
    identify_worker_->wait();
    identify_worker_.reset();
    identify_link_.reset();
    set_identification_in_progress(false);
    if (auto done = std::exchange(connect_done_, {}); done)
    {
        done(false);
    }
}

void MainWindow::set_identification_in_progress(bool in_progress)
{
    log_transport_list->setEnabled(!in_progress);
    for (QMenu *menu : ui->menubar->findChildren<QMenu *>())
    {
        for (QAction *action : menu->actions())
        {
            if (action->text() == "Connect" || action->text() == "Logging")
            {
                action->setEnabled(!in_progress);
            }
        }
    }
}
```

At the top of `MainWindow::disconnect_from_ecu()`, before `qDebug() << "Disconnecting...";`, add `stop_identification();`. At the top of `MainWindow::start_ecu_operations`, as its first statement, add `stop_identification();`.

In `src/ui/desktop/mainwindow.cpp`, make `stop_identification()` the first statement of `MainWindow::~MainWindow()`, preceded by `connect_done_ = nullptr;`. No continuation may run while the window is being destroyed.

`std::exchange` comes from `<utility>`, which `menu_actions.cpp` already includes.

- [ ] **Step 5: Make the logging start a continuation**

In `MainWindow::toggle_realtime()`, replace

```cpp
        qDebug() << "Start datalog";
        if (!ecu_init_complete)
        {
            if (connect_to_ecu())
            {
                restoreLoggingUiState();
                QMessageBox::information(this, tr("ECU connection"), "Unable to connect to ECU");
                return;
            }
        }
        logging_state = true;
```

with

```cpp
        qDebug() << "Start datalog";
        if (!ecu_init_complete)
        {
            connect_to_ecu(
                [this](bool connected)
                {
                    if (!connected)
                    {
                        restoreLoggingUiState();
                        QMessageBox::information(this, tr("ECU connection"), "Unable to connect to ECU");
                        return;
                    }
                    continue_start_logging();
                });
            return;
        }
        continue_start_logging();
    }
    else
    {
        qDebug() << "Stop datalog";
        if (datalog_file_open)
        {
            datalog_file_open = false;
            datalog_file.close();
        }

        loggingEngine->stop();

        // disconnect_from_ecu();
    }
}

void MainWindow::continue_start_logging()
{
    using namespace std::chrono_literals;

    {
        logging_state = true;
```

This moves everything from `logging_state = true;` down to the start-failure checks into `continue_start_logging`. Then delete the original `else { ... "Stop datalog" ... }` block that follows it, which now appears twice. Keep the copy inserted above and delete the old one at the end of the former function. The new function's final lines must read:

```cpp
        const auto started = loggingEngine->start(config, std::move(*snapshot));
        if (!started)
        {
            restoreLoggingUiState();
            QMessageBox::information(this, tr("Logging"), "Unable to start logging");
            return;
        }
    }
}
```

The extra inner braces keep the moved block's indentation unchanged. That keeps this diff reviewable; clang-format leaves them alone.

Step 3 already declared `continue_start_logging()` in the header.

- [ ] **Step 6: Delete the legacy SSM init code**

In `src/ui/desktop/log_operations_ssm.cpp`, delete `#include "src/platform/desktop/common/serial/serial_port_actions.h"` and the whole definitions of `MainWindow::ecu_init()`, `MainWindow::ssm_init()`, `MainWindow::ssm_kline_init()`, `MainWindow::ssm_can_init()`, `MainWindow::add_ssm_header(...)`, and `MainWindow::calculate_checksum(...)`. What remains is `parse_log_value_list` and `log_to_file`. Keep the file name; renaming it would bury this diff.

In `src/ui/desktop/mainwindow.cpp`, delete the whole definition of `QString MainWindow::parse_ecuid(QByteArray received)`, and delete the line `serial = &connection->facade();` in the constructor.

Run: `grep -n "ecu_init()\|ssm_init\|ssm_kline_init\|ssm_can_init\|add_ssm_header\|calculate_checksum\|parse_ecuid\|ecu_init_started\|serial->" src/ui/desktop/*.cpp src/ui/desktop/*.h`
Expected: only commented-out lines in `mainwindow.cpp` (the `// connect(ssm_init_poll_timer, ... ecu_init()` block and `// serial->...` comments), plus DataTerminal's own `add_ssm_header` in `dataterminal.cpp`/`dataterminal.h`, which is a different class.

- [ ] **Step 7: Run the tests**

Run: `bazel test --config=release //src/ui/desktop:all //apps/desktop:all`
Expected: PASS, including the five new tests and the existing `loggingCapturesTargetForEachRun`, which sets `ecu_init_complete = true` and so takes the synchronous `continue_start_logging()` path.

- [ ] **Step 8: Commit**

```bash
git add src/ui/desktop
git commit -F - <<'EOF'
refactor(ui): identify the ECU off the UI thread on connect (step 6h-4)

connect_to_ecu opens the port and hands Subaru identification to
SsmIdentifyWorker; logging start continues once it completes. Other
makes skip the 2.5 s of empty retries. Disconnect, flash operations,
and window teardown stop a running identification, and a completion
from a stopped worker is dropped. The legacy ssm_init, ssm_kline_init,
ssm_can_init, and their helpers are deleted.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 12: Remove `//src/ui/desktop` from the `serial_qt_compat` allowlist

**Files:**
- Modify: `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/menu_actions.cpp`, `src/ui/desktop/BUILD.bazel`
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`, `scripts/check-serial-compat-allowlist.py`

**Interfaces:**
- Consumes: Tasks 8–11. After them, no `:desktop` source calls the facade.
- Produces: `serial_qt_compat` visible to `//src/platform/desktop/common/serial:__pkg__`, `//src/platform/desktop/common/transport:__pkg__`, and `//tests:__pkg__` only.

- [ ] **Step 1: Shrink the list first, so the build proves the rest**

In `src/platform/desktop/common/serial/BUILD.bazel`, delete the line `"//src/ui/desktop:__pkg__",` from `serial_qt_compat`'s `visibility`. In `scripts/check-serial-compat-allowlist.py`, delete `"//src/ui/desktop:__pkg__",` from `FROZEN`.

- [ ] **Step 2: Run the build to see what still reaches the facade**

Run: `bazel build --config=release //src/ui/desktop:all`
Expected: FAIL with a visibility error. `:desktop` still lists `//src/platform/desktop/common/serial:serial_qt_compat` in its `deps`.

- [ ] **Step 3: Drop the dependency and the includes**

In `src/ui/desktop/BUILD.bazel`, delete `"//src/platform/desktop/common/serial:serial_qt_compat",` from `:desktop`'s `deps`. Delete `#include "src/platform/desktop/common/serial/serial_port_actions.h"` from `src/ui/desktop/mainwindow.cpp` and `src/ui/desktop/menu_actions.cpp`.

- [ ] **Step 4: Verify**

Run:

```bash
grep -rn "serial_port_actions.h" src/ui/desktop
bazel build --config=release //:fastecu
bazel test --config=release //:serial_compat_allowlist //src/ui/desktop:all //apps/desktop:all
```

Expected: the `grep` prints nothing. The build passes. `//:serial_compat_allowlist` passes and prints `OK: 3 entries, none added.` If the build fails on a missing declaration, a file was using a type that came in only through the facade header. Include that type's own header, and do not re-add the dependency.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop src/platform/desktop/common/serial/BUILD.bazel scripts/check-serial-compat-allowlist.py
git commit -F - <<'EOF'
build: drop //src/ui/desktop from the serial_qt_compat allowlist (step 6h-4)

No MainWindow source includes serial_port_actions.h any more: it talks to
AdapterConnection and hands the facade on as a forward-declared
reference. Three entries remain: the serial package itself, transport,
and //tests.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 13: Close-out documentation

**Files:**
- Modify: `docs/design-notes.md`, `docs/modularization-plan.md`, `docs/tech-debt.md`
- Create: `docs/connection-bench-checklist.md`
- Delete: `docs/superpowers/specs/2026-09-27-step6h-connection-identification-design.md`, `docs/superpowers/plans/2026-09-27-step6h-connection-identification.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the lasting record of 6h; the temporary spec and plan are removed, as 6g's were.

- [ ] **Step 1: Add the design-notes section**

In `docs/design-notes.md`, insert this section directly above `## Testing`:

```markdown
## Connection and identification

### `AdapterConnection` is a concrete adapter, not a port

`MainWindow`'s connection handling -- port listing, opening, the
log-transport flag profile, idle resets, battery voltage -- is desktop-facade
detail: port-name strings, OpenPort-only voltage, remote-replica waits.
Android's first target, MUT/DMA logging over USB, would reuse almost none of
it. So `//src/platform/desktop/common/connection:adapter_connection` is a
concrete Qt class `MainWindow` calls directly, and each member reproduces the
facade call sequence it replaced, pinned by `adapter_connection_test`. The
portable part is the protocol: `identify_ssm_ecu` in `//src/backend/diagnostics`.
If a later platform needs connection lifecycle, a port can be cut from the
adapter's surface then.

### The two idle resets stay separate

`clear_link_flags` (a port refresh) clears every link flag; `return_to_idle`
(a disconnect) resets baud and parity and leaves the flags alone.
`connect_to_ecu` does not reapply the flags `log_transport_changed` set, so a
disconnect that cleared them would make the next connect on a CAN transport
open as K-Line. A port refresh still leaves parity as it found it -- pinned,
not fixed.

### SSM2 identification is validated the way RomRaider validates it

RomRaider's `SSMResponseProcessor.validateResponse` is the reference: header
`0x80`, tester `0xF0`, the requested target, response code `0xFF`, and the
checksum. The legacy code checked only the length, so a corrupted frame
became a wrong ECU ID and wrong log capabilities. The frame is first cut to
its declared length, because the legacy code accepted trailing bytes.
SSM1 keeps its length-only check: RomRaider has no SSM1, and its checks are
SSM2's.

### There is no write that skips the echo check

`IDiagnosticLink` has one write, echo-checked. SSM1 was the only caller of a
plain `write_serial_data`, for two of its three wake-up writes; they are
echo-checked now. RomRaider has no such write either -- it strips the K-Line
echo from the response by length. This is a bench-gated behavior change.

### Connect is asynchronous

`SsmIdentifyWorker` runs the five-attempt loop off the UI thread. While it
runs, the log-transport combo and the Connect and Logging actions are
disabled; every other entry point that touches the facade (disconnect, flash
operations, window teardown) stops it first. A completion queued by a worker
that was stopped is recognised by a generation counter and dropped, because
Qt still delivers an event posted before its sender died. Logging start
waits through a continuation. A connect still counts as successful when the
port opened and identification failed, as before.

### Pinned quirks

- A connect succeeds when the port opens, even if identification fails.
- Raw-CAN identification does nothing: its legacy branch tested a protocol
  value no configuration sets.
- SSM1 is checked by length only.
- CAN identification uses UDS `22 F1 82`, not SSM `AA`, so CAN logging gets
  no capability bits (see the [tech-debt roadmap](tech-debt.md)).
- `log_transport_changed` sets 29-bit identifiers for iso15765 and 11-bit for
  raw CAN.
- A port refresh leaves parity unchanged.

### Behavior changes

1. The developer toggles `can_listener`, `simulate_obd`, and
   `test_haltech_ic7_display` are deleted. None was ever in the shipped
   `menu.cfg`; each looped forever on the UI thread.
2. SSM identification runs off the UI thread and can be cancelled.
3. SSM2 init responses are validated; trailing bytes are dropped.
4. SSM1's two plain writes are echo-checked.
5. K-Line SSM2 identification opens the link itself with the settings it
   used to inherit.
6. Every K-Line `IDiagnosticLink::open` now sets parity explicitly, so an
   even parity left by SSM1 no longer leaks into a later DTC or BIU session.
7. BIU opens directly at 10400 instead of opening and then changing speed.
8. A non-Subaru connect -- every Mitsubishi MUT/DMA logging start -- skips
   2.5 s of empty retries.
9. Short or malformed init frames fail as `BadResponse` instead of reading
   out of range.
10. SSM1's trailing drain stops after 100 reads, and a trailing frame too
    short for an ID is ignored instead of becoming a truncated ID.
11. With no serial port present, the DTC, BIU, and terminal commands warn
    instead of indexing past the end of the port list.
```

- [ ] **Step 2: Update the modularization plan**

In `docs/modularization-plan.md`, in the **Status** section, replace

```markdown
are complete — see below. **6g (diagnostic tools) is complete**, pending
bench qualification. Next is **6h**: the `MainWindow` connection service and
SSM ECU identification, plus a decision on the dev toggles `can_listener`,
`simulate_obd`, and `test_haltech_ic7_display`. Step 7 (Android seam) has not
started.
```

with

```markdown
are complete — see below. **6g (diagnostic tools)** and **6h (connection and
SSM identification)** are complete, pending bench qualification. Next is
**6i**: delete `serial_qt_compat`, whose visibility list now holds only the
serial package itself, `transport`, and `//tests`, and fold its sources into
the owning packages. Step 7 (Android seam) has not started.
```

Then, directly after the whole `- **6g diagnostic tools — complete**, ...` bullet in step 6, insert:

```markdown
   - **6h connection and SSM identification — complete**, pending bench
     qualification (see the
     [connection bench checklist](connection-bench-checklist.md)). No
     `//src/ui/desktop` source includes `serial_port_actions.h`, and
     `//src/ui/desktop:__pkg__` is gone from the `serial_qt_compat`
     allowlist and `FROZEN`. `MainWindow` talks to a concrete platform
     adapter, `AdapterConnection` (`//src/platform/desktop/common/connection`),
     and hands the facade on only as a forward-declared reference. SSM ECU
     identification is a portable `identify_ssm_ecu` in
     `//src/backend/diagnostics`, run off the UI thread by `SsmIdentifyWorker`;
     SSM2 init responses are validated the way RomRaider validates them.
     `KlineLinkConfig` gained a parity field. The unshipped developer toggles
     `can_listener`, `simulate_obd`, and `test_haltech_ic7_display` were
     deleted. Five PRs, not yet numbered (6h-0 spec and plan, 6h-1 toggles,
     6h-2 identification, 6h-3 adapter, 6h-4 worker and close-out). See the
     [design notes](design-notes.md#connection-and-identification).
```

- [ ] **Step 3: Update the tech-debt roadmap**

In `docs/tech-debt.md`, in `### P1: Drain the serial_qt_compat allowlist`, replace

```markdown
shrink, never grow. It currently holds 4 entries: `//src/ui/desktop:__pkg__`
under UI (0 in backend), plus `//src/platform/desktop/common/serial:__pkg__`
(the package itself), `//src/platform/desktop/common/transport:__pkg__`, and
`//tests:__pkg__`. Step 6g removed `//src/ui/desktop/biu:__pkg__`: BIU now
reaches the facade through `IDiagnosticLink`/`SerialDiagnosticLink` instead.
`//src/ui/desktop:__pkg__` is step 6h's to remove, once `MainWindow`'s
connection orchestration no longer needs the full facade directly. The step
5 tail's wave 7 deleted the `//src/platform/desktop/common/flash/legacy`
entry along with the package it named.
```

with

```markdown
shrink, never grow. It currently holds 3 entries, none of them UI or
backend: `//src/platform/desktop/common/serial:__pkg__` (the package itself),
`//src/platform/desktop/common/transport:__pkg__`, and `//tests:__pkg__`.
Step 6g removed `//src/ui/desktop/biu:__pkg__`, and step 6h removed
`//src/ui/desktop:__pkg__`: `MainWindow` now talks to `AdapterConnection`.
The step 5 tail's wave 7 deleted the `//src/platform/desktop/common/flash/legacy`
entry along with the package it named. Deleting the target itself is step 6i.
```

Directly above `### P2: Convert suppressed signed-bitwise arithmetic to unsigned operands`, insert:

```markdown
### P2: Identify Subaru CAN ECUs with SSM `AA`

Step 6h kept CAN identification byte-faithful: iso15765 sends UDS
`22 F1 82` and gets an ID but no capability bits, so CAN logging never
filters log values by what the ECU supports, and raw CAN identifies nothing
(its legacy branch tested a protocol value no configuration sets). RomRaider
identifies over CAN with SSM `AA` to 0x7E0 and gets `EA` plus the same
capability bytes K-Line returns.

Actions:

- Capture an `AA`/`EA` exchange on a bench CAN ECU before changing anything.
- Add an `SsmVariant` for it in `identify_ssm_ecu`, validated like SSM2, and
  route both CAN transports to it.
- Qualify it on the [connection bench checklist](connection-bench-checklist.md).
```

Run: `grep -n "serial_qt_compat\|4 entries\|step 6h" docs/tech-debt.md`
Expected: no other line still says the allowlist holds 4 entries or that 6h is pending. Fix any that does, in the same words as above.

- [ ] **Step 4: Create the bench checklist**

Create `docs/connection-bench-checklist.md`:

```markdown
# Connection and identification -- bench verification checklist

Step 6h moved `MainWindow`'s connection handling onto `AdapterConnection`
and its SSM ECU identification into a portable `identify_ssm_ecu`, run off
the UI thread by `SsmIdentifyWorker`. Most facade call sequences are
unchanged and pinned by tests, but several wire-level behaviors changed on
purpose (see the [design notes](design-notes.md#connection-and-identification)).
Automated tests (`ssm_identify_test.cpp`, `ssm_identify_worker_test.cpp`,
`adapter_connection_test.cpp`, `serial_diagnostic_link_test.cpp`,
`mainwindow_test.cpp`) are regression evidence, not hardware qualification.

No row below is qualified until it is run on a bench and signed off. This
checklist does not affect the [flash qualification matrix](flash-qualification-matrix.md);
the only flash-path change is that a flash operation now stops a running
identification first.

Run `bazel test --config=release //...` first.

## Prerequisites

- A Subaru bench ECU and TCU reachable over K-Line (SSM2), through OpenPort
  2.0 and through a direct K-Line cable.
- A Subaru bench ECU on one of the two `SSM`-transport (SSM1) entries in
  `protocols.cfg`.
- A Subaru bench ECU reachable over iso15765.
- A Mitsubishi bench ECU with MUT/DMA logging.
- Build revision, OS, and adapter/driver version recorded in the run record
  below before starting.

## Run record

| Field | Value |
| --- | --- |
| Build revision | Not yet tested |
| OS and version | Not yet tested |
| Adapter and driver version | Not yet tested |
| ECU/TCU and protocol | Not yet tested |
| Operator and date | Not yet tested |
| Observed result and trace location | Not yet tested |

## Checks

| # | Check | Expected | Result |
| --- | --- | --- | --- |
| 1 | Connect, Subaru, K-Line, ECU selected, OpenPort 2.0 | Status bar shows the ECU ID; logboxes reflect the ECU's capability bits; the window stays responsive during identification | Not yet tested |
| 2 | Connect, Subaru, K-Line, ECU selected, direct K-Line cable | Same as #1 | Not yet tested |
| 3 | Connect, Subaru, K-Line, TCU selected | Status bar shows the TCU ID; request target byte is `0x18` | Not yet tested |
| 4 | Connect, Subaru, K-Line, with the wrong unit selected (TCU radio on an ECU-only bench) | Each attempt logs a `target id` validation failure; after five attempts the connection is dropped and the controls are re-enabled | Not yet tested |
| 5 | Connect, Subaru, `SSM` transport (SSM1) | Status bar shows the ECU ID, with the now echo-checked wake-up writes; record the wire trace | Not yet tested |
| 6 | Connect, Subaru, iso15765 | Status bar shows the `F182` ID | Not yet tested |
| 7 | Start logging, Mitsubishi MUT/DMA, not yet connected | Logging starts with no 2.5 s pause beforehand; values update | Not yet tested |
| 8 | Start logging, Subaru K-Line, not yet connected | Logging starts after identification completes | Not yet tested |
| 9 | Disconnect during identification (#1 with the ECU unpowered) | Identification stops within a second; controls re-enabled; no ECU ID appears afterwards | Not yet tested |
| 10 | Close the window during identification | The application exits promptly without hanging | Not yet tested |
| 11 | BIU window | The link opens at 10400 on the selected port; BIU commands answer | Not yet tested |
| 12 | DTC and DataTerminal windows | Both open on the selected port and communicate as before | Not yet tested |
| 13 | DTC, BIU, and DataTerminal with no adapter connected (empty port list) | Each shows "No serial port selected!" and nothing crashes | Not yet tested |
| 14 | Port refresh and log-transport switching, then connect | The newly chosen port and transport are used | Not yet tested |
| 15 | Read ROM on an OpenPort 2.0 | Battery voltage updates in the flash window during the operation | Not yet tested |
| 16 | SSM1 connect, then a DTC read over K-Line | The DTC session opens with no parity (previously it could inherit SSM1's even parity) | Not yet tested |
```

- [ ] **Step 5: Delete the temporary spec and plan**

```bash
git rm docs/superpowers/specs/2026-09-27-step6h-connection-identification-design.md docs/superpowers/plans/2026-09-27-step6h-connection-identification.md
```

- [ ] **Step 6: Verify and commit**

Run: `prek run --all-files`
Expected: all hooks pass, including `lychee`, which checks the new links.

```bash
git add docs
git commit -F - <<'EOF'
docs: close out step 6h (connection and SSM identification)

Record the adapter, identification, and async-connect decisions in the
design notes, mark 6h complete in the modularization plan, update the
serial_qt_compat allowlist entry, add the SSM-AA CAN identification debt,
and add the connection bench checklist. Remove the temporary spec and
plan.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
EOF
```

### Task 14: Final verification and the PR stack

**Files:** none.

- [ ] **Step 1: Run the full gate on the top of the stack**

Run:

```bash
bazel build --config=release //:fastecu
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Expected: every command passes. Record the passed/skipped test counts for the PR descriptions. The Windows-only `//tests:serial_backend_tests` flake is known and pre-existing: rerun it before blaming this stack.

- [ ] **Step 2: Rebuild each lower branch**

For each branch from `refactor/step6h-1-dev-toggles` through `refactor/step6h-3-adapter-connection`, check it out and run `bazel build --config=release //:fastecu && bazel test --config=release //...`. Every PR in the stack must be green on its own.

- [ ] **Step 3: Ask before publishing**

Stop and ask the user for authorization to push. Do not push without it.

- [ ] **Step 4: Publish the stack (after authorization)**

```bash
gh stack init --base master docs/step6h-connection-identification refactor/step6h-1-dev-toggles refactor/step6h-2-ssm-identify refactor/step6h-3-adapter-connection refactor/step6h-4-connect-worker
gh stack submit --auto
```

Each PR description says what the PR changes, lists its behavior changes from the spec, records the Step 1 counts, and for 6h-2 lists the seven mutation-check outcomes from Task 4 Step 5. Each description ends with:

```
🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01JTGxwfrpdX8BugpeWowZCn
```

The hand-off to the user mentions `gh stack merge` and `gh stack sync` for landing the stack.
