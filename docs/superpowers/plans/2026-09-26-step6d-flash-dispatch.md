# Step 6d Flash-Operation Dispatch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move flash dispatch out of `MainWindow::start_ecu_operations` into portable helpers, a platform serial helper, and a UI `FlashOperationController`. Replace the `goto` with a scope guard, and fix two defects: write-path early returns skip cleanup, and failed reads leak the read slot.

**Architecture:**
- Pure decisions go to `//src/backend/flash:flash_operation_request`, which is portable and tested with gtest.
- The duplicated idle-line serial reset goes to `//src/platform/desktop/common/serial:serial_idle`.
- Denso TCU routing, workflow creation, `FlashDialog`, and the unknown-protocol warning go to `//src/ui/desktop/flash/operation:flash_operation_controller`.
- `MainWindow` keeps preflight, checksum correction, and the calibration handoff.
- The first task makes `test_mainwindow` actually fail on Google Mock violations, since every later task relies on it.

**Tech Stack:** C++23, Qt 6 (Widgets, QtTest, `QScopeGuard`), GoogleTest/GoogleMock, Bazel 9, prek, `gh stack`.

**Spec:** [docs/superpowers/specs/2026-09-26-step6d-flash-dispatch-design.md](../specs/2026-09-26-step6d-flash-dispatch-design.md)

## Global Constraints

- Bazel is the only build graph; every command runs with `--config=release`.
- Never add an entry to `serial_qt_compat`'s `visibility` list, to `FROZEN` in `scripts/check-serial-compat-allowlist.py`, or to `//bazel/qt:qt_layer`.
- A new portable target is registered by name in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`).
- No wire sequence, dialog text, dialog title, or prompt order changes, except for the two behavior changes in the spec.
- `qt_cc_library`: `Q_OBJECT` headers go in `hdrs`, other headers in `normal_hdrs`, and `hdrs = []` is required when there is no `Q_OBJECT` header.
- A QtTest suite that uses Google Mock calls `::testing::InitGoogleMock(&argc, argv)` in `main` and returns non-zero when `::testing::Test::HasFailure()`.
- Work lands as four PRs in a `gh stack` (6d-1 to 6d-4), each on its own branch, stacked on the previous one.
- Commit messages end with the attribution lines from the session's system reminder.
- Before every push: `prek run --all-files`, `bazel test --config=release //...`, and `bazel run //:clang_tidy_report_changed` all pass.

## Review Focus

1. **Denso TCU "Dump" choice.** It must fall through to the ROM-dump flash workflow, not stop as a handled service action. Only the cancelled and relearn-declined choices are tested. This is not automatable without a scripted TCU; the reviewer checks `FlashOperationController::run`'s `Dump` path by reading it.
2. **Write with a checksum module.** It must still run checksum correction before dispatch and restore the pre-checksum `FullRomData` afterwards. This code is unchanged but moves inside the scope guard. The reviewer confirms the restore still runs on every write outcome.
3. **Successful read.** It must keep its calibration (no slot release), name the file, build the trees, advance `ecuCalDefIndex`, and save. There is no automated success path; the reviewer confirms `release_read_slot` is cleared before the release check.
4. **Make other than Subaru/Mitsubishi.** It must skip dispatch but still run cleanup. Pinned by `otherMakesSkipDispatchButStillRunCleanup` in Task 3.
5. **Cancel on the checksum warning.** It must stop battery polling like "No file selected!" does. Only the "No file selected!" return is tested (Task 3). The checksum-cancel return is the same `return 0` under the same guard, and the reviewer confirms nothing between the two returns bypasses the guard.

---

## PR 6d-1

```bash
# docs/step6d-flash-dispatch-spec is master plus the spec and plan commits
git switch -c test/step6d-1-enforce-gmock-and-helpers docs/step6d-flash-dispatch-spec
```

### Task 1: Make `test_mainwindow` fail on Google Mock violations

**Files:**
- Modify: `src/ui/desktop/mainwindow_test.cpp` (`main`; the Denso TCU case's expectations)

**Interfaces:**
- Consumes: nothing.
- Produces: an enforcing `test_mainwindow` that every later task relies on.

- [ ] **Step 1: Enforce Google Mock in `main` (this is the failing "test")**

Replace `main` in `src/ui/desktop/mainwindow_test.cpp` with:

```cpp
int main(int argc, char **argv)
{
    std::fprintf(stderr, "MainWindowTest: entered main\n");
    ::testing::InitGoogleMock(&argc, argv);
    QApplication app(argc, argv);
    std::fprintf(stderr, "MainWindowTest: QApplication initialized\n");
    MainWindowTest test;
    const int result = QTest::qExec(&test, argc, argv);
    std::fprintf(stderr, "MainWindowTest: qExec returned %d\n", result);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow --test_output=errors`
Expected: FAIL. The log shows `mainwindow_test.cpp:…: Failure` blocks for the Denso TCU case: `reset_connection()` "Expected: to be never called", and `read_vbatt()` / `set_use_openport2_adapter(true)` "never called - unsatisfied".

- [ ] **Step 3: Pin the calls that actually happen**

In `handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling`, replace

```cpp
        EXPECT_CALL(*fake, set_use_openport2_adapter(true)).WillOnce(::testing::DoDefault());
        EXPECT_CALL(*fake, read_vbatt()).WillOnce(::testing::Return(12500UL));
```

with nothing (neither call happens), and replace

```cpp
        if (choice.isEmpty())
        {
            EXPECT_CALL(*fake, reset_connection()).Times(0);
            EXPECT_CALL(*fake, change_port_speed(::testing::_)).Times(0);
        }
        else
        {
            ::testing::InSequence sequence;
            EXPECT_CALL(*fake, reset_connection()).WillOnce(::testing::Return());
            EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(STATUS_SUCCESS));
        }
```

with

```cpp
        // Characterization: the call counts observed on master (before step
        // 6d) for both rows, pinned so the dispatch refactor cannot change them.
        EXPECT_CALL(*fake, reset_connection()).Times(3);
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(2);
```

- [ ] **Step 4: Run it and confirm it passes**

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow --test_output=errors`
Expected: PASS, and `grep -c ': Failure' bazel-testlogs/src/ui/desktop/test_mainwindow/test.log` prints `0`. If a row reports a different count, pin the count the failure message reports (this is characterization). Record it as a ruling and correct the comment. Do not loosen the count to `AtLeast`.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop/mainwindow_test.cpp
git commit -m "test(ui): make test_mainwindow fail on Google Mock violations (step 6d-1)"
```

### Task 2: Portable flash-operation helpers

**Files:**
- Create: `src/backend/flash/flash_operation_request.h`, `src/backend/flash/flash_operation_request.cpp`, `src/backend/flash/flash_operation_request_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`, `bazel/portable_targets.bzl`, `src/ui/desktop/BUILD.bazel` (`:desktop` deps), `src/ui/desktop/mainwindow.cpp` (`start_ecu_operations`)

**Interfaces:**
- Consumes: `fastecu::flash::FlashOperation` (`src/backend/flash/flash_types.h`: `Read`, `Write`, `TestWrite`).
- Produces (namespace `fastecu::flash`, header `src/backend/flash/flash_operation_request.h`, target `//src/backend/flash:flash_operation_request`):
  - `FlashOperation flash_operation_from_command(std::string_view command);`
  - `bool is_denso_tcu_protocol(std::string_view protocol);`
  - `std::string kernel_path(std::string_view dir, std::string_view file);`
  - `std::string read_image_filename(std::string_view rom_id, std::string_view timestamp);`

- [ ] **Step 1: Write the failing test**

Create `src/backend/flash/flash_operation_request_test.cpp`:

```cpp
#include "src/backend/flash/flash_operation_request.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

TEST(FlashOperationFromCommand, MapsTheTwoWriteCommands)
{
    EXPECT_EQ(flash_operation_from_command("write"), FlashOperation::Write);
    EXPECT_EQ(flash_operation_from_command("test_write"), FlashOperation::TestWrite);
}

TEST(FlashOperationFromCommand, TreatsEveryOtherCommandAsRead)
{
    EXPECT_EQ(flash_operation_from_command("read"), FlashOperation::Read);
    EXPECT_EQ(flash_operation_from_command(""), FlashOperation::Read);
    EXPECT_EQ(flash_operation_from_command("Write"), FlashOperation::Read);
    EXPECT_EQ(flash_operation_from_command("test-write"), FlashOperation::Read);
}

TEST(IsDensoTcuProtocol, MatchesBothDensoTcuCanProtocols)
{
    EXPECT_TRUE(is_denso_tcu_protocol("sub_tcu_denso_sh7055_can"));
    EXPECT_TRUE(is_denso_tcu_protocol("sub_tcu_denso_sh7058_can"));
}

TEST(IsDensoTcuProtocol, RejectsNearMisses)
{
    EXPECT_FALSE(is_denso_tcu_protocol("sub_tcu_denso_sh7058_can_future"));
    EXPECT_FALSE(is_denso_tcu_protocol("sub_ecu_denso_sh7058_can"));
    EXPECT_FALSE(is_denso_tcu_protocol("sub_tcu_denso_sh7055"));
    EXPECT_FALSE(is_denso_tcu_protocol(""));
}

TEST(KernelPath, InsertsASeparatorOnlyWhenMissing)
{
    EXPECT_EQ(kernel_path("/k", "a.bin"), "/k/a.bin");
    EXPECT_EQ(kernel_path("/k/", "a.bin"), "/k/a.bin");
}

TEST(KernelPath, HandlesEmptyParts)
{
    EXPECT_EQ(kernel_path("", "a.bin"), "a.bin");
    EXPECT_EQ(kernel_path("/k", ""), "/k/");
}

TEST(ReadImageFilename, PrefixesTheRomId)
{
    EXPECT_EQ(read_image_filename("A2WC522N", "2026-09-26_08h00m00s"), "A2WC522N2026-09-26_08h00m00s.bin");
}

TEST(ReadImageFilename, FallsBackToReadImageWithoutARomId)
{
    EXPECT_EQ(read_image_filename("", "2026-09-26_08h00m00s"), "read_image_2026-09-26_08h00m00s.bin");
}

} // namespace
} // namespace fastecu::flash
```

Add to `src/backend/flash/BUILD.bazel`, after the `flash_executor_test` target:

```python
cc_library(
    name = "flash_operation_request",
    srcs = ["flash_operation_request.cpp"],
    hdrs = ["flash_operation_request.h"],
    deps = [":flash_types"],
)

fastecu_portable_gtest(
    name = "flash_operation_request_test",
    srcs = ["flash_operation_request_test.cpp"],
    deps = [":flash_operation_request"],
)
```

In `bazel/portable_targets.bzl`, add `"flash_operation_request",` to the `"src/backend/flash"` list between `"flash_executor",` and `"flash_plan",`.

- [ ] **Step 2: Run it and confirm it fails**

Run: `bazel test --config=release //src/backend/flash:flash_operation_request_test`
Expected: FAIL: `flash_operation_request.h` / `.cpp` missing.

- [ ] **Step 3: Implement**

Create `src/backend/flash/flash_operation_request.h`:

```cpp
#pragma once

#include <string>
#include <string_view>

#include "src/backend/flash/flash_types.h"

namespace fastecu::flash
{

// The desktop menu's command strings: "write" and "test_write" write;
// anything else, including "read", reads.
FlashOperation flash_operation_from_command(std::string_view command);

// The two Denso TCU CAN protocols whose reads first offer service functions.
// Exact match: suffixed future protocols are not TCU reads.
bool is_denso_tcu_protocol(std::string_view protocol);

// dir + file, inserting '/' when a non-empty dir does not end with one.
std::string kernel_path(std::string_view dir, std::string_view file);

// "<rom_id><timestamp>.bin", or "read_image_<timestamp>.bin" when rom_id is empty.
std::string read_image_filename(std::string_view rom_id, std::string_view timestamp);

} // namespace fastecu::flash
```

Create `src/backend/flash/flash_operation_request.cpp`:

```cpp
#include "src/backend/flash/flash_operation_request.h"

namespace fastecu::flash
{

FlashOperation flash_operation_from_command(std::string_view command)
{
    if (command == "write")
    {
        return FlashOperation::Write;
    }
    if (command == "test_write")
    {
        return FlashOperation::TestWrite;
    }
    return FlashOperation::Read;
}

bool is_denso_tcu_protocol(std::string_view protocol)
{
    return protocol == "sub_tcu_denso_sh7055_can" || protocol == "sub_tcu_denso_sh7058_can";
}

std::string kernel_path(std::string_view dir, std::string_view file)
{
    std::string path{dir};
    if (!path.empty() && path.back() != '/')
    {
        path.push_back('/');
    }
    path.append(file);
    return path;
}

std::string read_image_filename(std::string_view rom_id, std::string_view timestamp)
{
    std::string name = rom_id.empty() ? std::string{"read_image_"} : std::string{rom_id};
    name.append(timestamp);
    name.append(".bin");
    return name;
}

} // namespace fastecu::flash
```

- [ ] **Step 4: Run it and confirm it passes**

Run: `bazel test --config=release //src/backend/flash:flash_operation_request_test //:portable_closure`
Expected: PASS (8 tests), and `portable_closure` passes.

- [ ] **Step 5: Adopt the helpers in `MainWindow`**

Add `"//src/backend/flash:flash_operation_request",` to `:desktop`'s `deps` in `src/ui/desktop/BUILD.bazel`, and `#include "src/backend/flash/flash_operation_request.h"` to `src/ui/desktop/mainwindow.cpp` after the `flash_device_lookup.h` include. Then, in `start_ecu_operations`:

1. In both the write branch and the read branch, replace

```cpp
            ecuCalDef[rom_number]->Kernel =
                configValues->kernel_files_directory +
                configValues->flash_protocol_kernel.at(
                    configValues->flash_protocol_selected_id
                        .toInt()); // check_kernel(ecuCalDef[rom_number]->RomInfo.at(fileActions->FlashMethod));
```

with

```cpp
            ecuCalDef[rom_number]->Kernel = QString::fromStdString(fastecu::flash::kernel_path(
                configValues->kernel_files_directory.toStdString(),
                configValues->flash_protocol_kernel.at(configValues->flash_protocol_selected_id.toInt()).toStdString()));
```

2. Replace the `const fastecu::flash::FlashOperation operation = [&cmd_type] { … }();` lambda with

```cpp
        const fastecu::flash::FlashOperation operation =
            fastecu::flash::flash_operation_from_command(cmd_type.toStdString());
```

3. Replace `const bool denso_tcu = protocol == "sub_tcu_denso_sh7055_can" || protocol == "sub_tcu_denso_sh7058_can";` with `const bool denso_tcu = fastecu::flash::is_denso_tcu_protocol(protocol);`.

4. In the post-read block, replace

```cpp
                if (ecuCalDef[ecuCalDefIndex]->RomId.length())
                {
                    ecuCalDef[ecuCalDefIndex]->FileName = ecuCalDef[ecuCalDefIndex]->RomId + dateTimeString + ".bin";
                }
                else
                {
                    ecuCalDef[ecuCalDefIndex]->FileName = "read_image_" + dateTimeString + ".bin";
                }
```

with

```cpp
                ecuCalDef[ecuCalDefIndex]->FileName = QString::fromStdString(fastecu::flash::read_image_filename(
                    ecuCalDef[ecuCalDefIndex]->RomId.toStdString(), dateTimeString.toStdString()));
```

- [ ] **Step 6: Run the affected suites**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //src/backend/flash/... //src/ui/... //:portable_closure --test_output=errors`
Expected: build succeeds and everything passes, including `test_mainwindow` with Google Mock now enforced.

- [ ] **Step 7: Commit**

```bash
git add src/backend/flash bazel/portable_targets.bzl src/ui/desktop/BUILD.bazel src/ui/desktop/mainwindow.cpp
git commit -m "feat(flash): portable flash-operation request helpers, adopted in MainWindow (step 6d-1)"
```

- [ ] **Step 8: Gate**

Run `prek run --all-files`, `bazel test --config=release //...`, `bazel run //:clang_tidy_report_changed`; all must pass.

---

## PR 6d-2

```bash
git switch -c refactor/step6d-2-serial-idle-cleanup   # from the 6d-1 branch
```

### Task 3: `reset_serial_to_idle` and the scope-exit cleanup

**Files:**
- Create: `src/platform/desktop/common/serial/serial_idle.h`, `serial_idle.cpp`, `serial_idle_test.cpp`
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`, `src/ui/desktop/BUILD.bazel`, `src/ui/desktop/mainwindow.cpp` (`start_ecu_operations`), `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- Consumes: `SerialPortActions` facade methods.
- Produces: `void fastecu::desktop::serial::reset_serial_to_idle(SerialPortActions& serial);` in `src/platform/desktop/common/serial/serial_idle.h`, target `//src/platform/desktop/common/serial:serial_idle` (visible to `//src/ui/desktop:__pkg__`).

- [ ] **Step 1: Write the failing helper test**

Create `src/platform/desktop/common/serial/serial_idle_test.cpp`:

```cpp
#include "src/platform/desktop/common/serial/serial_idle.h"

#include <QCoreApplication>
#include <QSerialPort>
#include <QTest>

#include <gmock/gmock.h>

#include <memory>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

class SerialIdleTest : public QObject
{
    Q_OBJECT

  private slots:
    void resetsTheConnectionThenRestoresTheIdleLineSettingsInOrder()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial{"", "", nullptr, nullptr, [&fake]() -> SerialBackend *
                                 {
                                     fake = new NiceFakeBackend;
                                     return fake;
                                 }};
        QVERIFY(serial.set_add_ssm_header(false)); // forces the backend into existence
        QVERIFY(fake != nullptr);

        {
            ::testing::InSequence sequence;
            EXPECT_CALL(*fake, reset_connection());
            EXPECT_CALL(*fake, set_is_iso14230_connection(false));
            EXPECT_CALL(*fake, set_is_29_bit_id(false));
            EXPECT_CALL(*fake, set_add_iso14230_header(false));
            EXPECT_CALL(*fake, set_is_can_connection(false));
            EXPECT_CALL(*fake, set_is_iso15765_connection(false));
            EXPECT_CALL(*fake, set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)));
            EXPECT_CALL(*fake, set_serial_port_baudrate(QStringLiteral("4800")));
        }

        fastecu::desktop::serial::reset_serial_to_idle(serial);
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    SerialIdleTest test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "serial_idle_test.moc"
```

Add to `src/platform/desktop/common/serial/BUILD.bazel`, after the `desktop_serial_factory_test` target:

```python
# The idle line state MainWindow restores before and after every ECU
# operation. It calls the facade, so it lives beside it; serial_qt_compat's
# frozen visibility list could not gain the calling UI package.
qt_cc_library(
    name = "serial_idle",
    srcs = ["serial_idle.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["serial_idle.h"],
    visibility = ["//src/ui/desktop:__pkg__"],
    deps = QT_DEPS + [":serial_qt_compat"],
)

fastecu_qttest(
    name = "serial_idle_test",
    size = "small",
    src = "serial_idle_test.cpp",
    deps = [
        ":serial_idle",
        ":serial_qt_compat",
        "//src/platform/desktop/common/serial/testing:fake_serial_backend",
    ],
)
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `bazel test --config=release //src/platform/desktop/common/serial:serial_idle_test`
Expected: FAIL: `serial_idle.h` / `.cpp` missing.

- [ ] **Step 3: Implement**

Create `src/platform/desktop/common/serial/serial_idle.h`:

```cpp
#pragma once

class SerialPortActions;

namespace fastecu::desktop::serial
{

// Resets the connection and restores the idle line state MainWindow uses
// between ECU operations: every protocol mode off, 11-bit ids, no parity,
// 4800 baud.
void reset_serial_to_idle(SerialPortActions& serial);

} // namespace fastecu::desktop::serial
```

Create `src/platform/desktop/common/serial/serial_idle.cpp`:

```cpp
#include "src/platform/desktop/common/serial/serial_idle.h"

#include <QSerialPort>

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::desktop::serial
{

void reset_serial_to_idle(SerialPortActions& serial)
{
    serial.reset_connection();
    serial.set_is_iso14230_connection(false);
    serial.set_is_29_bit_id(false);
    serial.set_add_iso14230_header(false);
    serial.set_is_can_connection(false);
    serial.set_is_iso15765_connection(false);
    serial.set_serial_port_parity(QSerialPort::NoParity);
    serial.set_serial_port_baudrate("4800");
}

} // namespace fastecu::desktop::serial
```

- [ ] **Step 4: Run it and confirm it passes**

Run: `bazel test --config=release //src/platform/desktop/common/serial:serial_idle_test //:serial_compat_allowlist`
Expected: PASS. The allowlist check passes unchanged.

- [ ] **Step 5: Write the failing `MainWindow` tests**

Add these two cases to `MainWindowTest` in `src/ui/desktop/mainwindow_test.cpp`, before `private:`:

```cpp
    // Spec behavior change 1: "No file selected!" returns after the entry
    // reset started battery polling; it must now run the cleanup.
    void writeWithoutASelectedCalibrationStopsVoltagePolling()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, write_serial_data(::testing::_)).Times(0);
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = "sub_ecu_denso_sh7058_can";
        window.configValues->flash_protocol_selected_mcu = "SH7058";
        window.configValues->flash_protocol_selected_id = "0";
        window.configValues->flash_protocol_kernel = {"test-kernel.bin"};
        window.configValues->flash_protocol_kernel_addr = {"0xFFFF3000"};
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";
        window.ui->calibrationFilesTreeWidget->clearSelection();

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "write"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QVERIFY(!window.vbatt_timer->isActive());
    }

    // Characterization: only Subaru and Mitsubishi dispatch, but every make
    // gets the cleanup.
    void otherMakesSkipDispatchButStillRunCleanup()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        FakeBackend *fake = services.fake;
        EXPECT_CALL(*fake, open_serial_port()).Times(0);
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).Times(::testing::AtLeast(1));
        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Nissan";
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(operation_driver.unexpectedFlashDialogCount(), 0);
        QVERIFY(!window.vbatt_timer->isActive());
    }
```

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow --test_output=errors`
Expected: FAIL in `writeWithoutASelectedCalibrationStopsVoltagePolling`, with `QVERIFY(!window.vbatt_timer->isActive())` failing and `change_port_speed("4800")` unsatisfied. `otherMakesSkipDispatchButStillRunCleanup` passes; it is characterization.

- [ ] **Step 6: Replace the `goto` with a scope guard**

Add `"//src/platform/desktop/common/serial:serial_idle",` to `:desktop`'s `deps`, and to `src/ui/desktop/mainwindow.cpp` add `#include <QScopeGuard>` after `#include <QSplashScreen>` and `#include "src/platform/desktop/common/serial/serial_idle.h"` after the `serial_port_actions.h` include.

In `start_ecu_operations`:

1. Directly after the first block

```cpp
    if (configValues->kernel_files_directory.at(configValues->kernel_files_directory.length() - 1) != '/')
    {
        configValues->kernel_files_directory.append("/");
    }
```

(the one before the make check), insert:

```cpp
    // Every path from here on — including the write-preflight early returns
    // and the Denso TCU service-action return — restores the serial facade
    // and stops battery polling on exit.
    const auto cleanup = qScopeGuard(
        [this]
        {
            vbatt_timer->stop();
            fastecu::desktop::serial::reset_serial_to_idle(*serial);
            ecuid.clear();
            ecu_init_complete = false;
            emit log_transport_list->currentIndexChanged(log_transport_list->currentIndex());
            serial->change_port_speed("4800");
        });
```

2. Inside the make check, replace

```cpp
        serial->reset_connection();
        ecuid.clear();
        ecu_init_complete = false;
        serial->set_is_iso14230_connection(false);
        serial->set_is_29_bit_id(false);
        serial->set_add_iso14230_header(false);
        serial->set_is_can_connection(false);
        serial->set_is_iso15765_connection(false);
        serial->set_serial_port_parity(QSerialPort::NoParity);
        serial->set_serial_port_baudrate("4800");
```

with

```cpp
        fastecu::desktop::serial::reset_serial_to_idle(*serial);
        ecuid.clear();
        ecu_init_complete = false;
```

3. Replace `                goto ecu_operation_cleanup;` with `                return 0;`.

4. Replace everything from the line `ecu_operation_cleanup:` to the end of the function with:

```cpp
    return 0;
}
```

Run: `grep -n "goto\|ecu_operation_cleanup" src/ui/desktop/mainwindow.cpp`
Expected: no matches.

- [ ] **Step 7: Run and confirm everything passes**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //src/ui/... //src/platform/desktop/common/serial:all //:serial_compat_allowlist --test_output=errors`
Expected: PASS. That includes both new cases and the Denso TCU case, still at 3 `reset_connection` and 2 `change_port_speed("4800")`.

- [ ] **Step 8: Commit**

```bash
git add src/platform/desktop/common/serial src/ui/desktop
git commit -m "refactor(ui): replace start_ecu_operations' goto with a scope guard (step 6d-2)"
```

- [ ] **Step 9: Gate**

Run `prek run --all-files`, `bazel test --config=release //...`, `bazel run //:clang_tidy_report_changed`; all must pass.

---

## PR 6d-3

```bash
git switch -c refactor/step6d-3-flash-operation-controller   # from the 6d-2 branch
```

### Task 4: `FlashOperationController`, the `MainWindow` switch, and the read-slot fix

**Files:**
- Create: `src/ui/desktop/flash/operation/BUILD.bazel`, `flash_operation_controller.h`, `flash_operation_controller.cpp`, `flash_operation_controller_test.cpp`
- Modify: `src/ui/desktop/BUILD.bazel`, `src/ui/desktop/mainwindow.h`, `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- Consumes: `flash_operation_from_command`, `is_denso_tcu_protocol`, `kernel_path` (Task 2); `reset_serial_to_idle` and the scope guard (Task 3); `FlashWorkflowFactory::tryCreate(FlashWorkflowRequest)`, `portableImageForOperation` (`src/platform/desktop/common/flash/flash_workflow.h`); `FlashDialog(std::unique_ptr<FlashWorkflow>, FlashOperation, const QString&, QWidget*)` and `FlashDialogResult{outcome, accepted_read_bytes, rom_id}` (`src/ui/desktop/flash/common/flash_dialog.h`); `choose_denso_tcu_read_action(QWidget*)`, `run_denso_tcu_service_action(DensoTcuReadAction, SerialPortActions*, std::string, QWidget*)`.
- Produces (namespace `fastecu::flash`, header `src/ui/desktop/flash/operation/flash_operation_controller.h`, target `//src/ui/desktop/flash/operation:flash_operation_controller`): `FlashOperationInput`, `FlashOperationStatus`, `FlashOperationOutcome`, `FlashOperationController` exactly as in the spec.

- [ ] **Step 1: Write the failing controller test**

Create `src/ui/desktop/flash/operation/flash_operation_controller_test.cpp`:

```cpp
#include "src/ui/desktop/flash/operation/flash_operation_controller.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QTest>
#include <QTimer>

#include <gmock/gmock.h>

#include <memory>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

namespace fastecu::flash
{
namespace
{

// Answers every message box: rejects the Denso TCU chooser, accepts anything
// else, and records the texts it saw.
class BoxDriver final : public QObject
{
    Q_OBJECT

  public:
    BoxDriver()
    {
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &BoxDriver::drive);
        timer_.start();
    }

    QStringList texts;

  private slots:
    void drive()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *box = qobject_cast<QMessageBox *>(widget); box != nullptr && box->isVisible())
            {
                texts << box->text();
                if (box->text() == "Choose which option")
                {
                    box->reject();
                }
                else
                {
                    box->accept();
                }
                return;
            }
        }
    }

  private:
    QTimer timer_;
};

std::unique_ptr<SerialPortActions> fakeSerial(FakeBackend **fake)
{
    auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                      [fake]() -> SerialBackend *
                                                      {
                                                          *fake = new NiceFakeBackend;
                                                          return *fake;
                                                      });
    if (!serial->set_add_ssm_header(false) || *fake == nullptr)
    {
        return nullptr;
    }
    return serial;
}

void expectNoEcuIo(FakeBackend& fake)
{
    EXPECT_CALL(fake, open_serial_port()).Times(0);
    EXPECT_CALL(fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(fake, read_serial_data(::testing::_)).Times(0);
}

} // namespace

class FlashOperationControllerTest : public QObject
{
    Q_OBJECT

  private slots:
    void unknownProtocolIsUnsupportedAndWarnsWithoutSerialIo()
    {
        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&fake);
        QVERIFY(serial != nullptr);
        expectNoEcuIo(*fake);
        FlashOperationController controller{*serial, nullptr};
        BoxDriver driver;

        const FlashOperationOutcome outcome = controller.run({
            .operation = FlashOperation::Read,
            .protocol = "sub_ecu_not_a_real_protocol",
            .mcu = "SH7058",
            .kernel_path = "/k/kernel.bin",
            .image = std::nullopt,
            .paths = {},
            .display_filename = "",
        });

        QCOMPARE(outcome.status, FlashOperationStatus::Unsupported);
        QVERIFY(!outcome.read_bytes.has_value());
        QCOMPARE(driver.texts,
                 QStringList{"Unknown flashmethod! Flashmethod \"sub_ecu_not_a_real_protocol\" not yet implemented!"});
    }

    void cancelledDensoTcuChooserIsHandledWithoutSerialIo()
    {
        FakeBackend *fake = nullptr;
        std::unique_ptr<SerialPortActions> serial = fakeSerial(&fake);
        QVERIFY(serial != nullptr);
        expectNoEcuIo(*fake);
        FlashOperationController controller{*serial, nullptr};
        BoxDriver driver;

        const FlashOperationOutcome outcome = controller.run({
            .operation = FlashOperation::Read,
            .protocol = "sub_tcu_denso_sh7058_can",
            .mcu = "SH7058",
            .kernel_path = "/k/tcu_kernel.bin",
            .image = std::nullopt,
            .paths = {},
            .display_filename = "",
        });

        QCOMPARE(outcome.status, FlashOperationStatus::ServiceActionHandled);
        QCOMPARE(driver.texts, QStringList{"Choose which option"});
    }
};

} // namespace fastecu::flash

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QApplication application(argc, argv);
    fastecu::flash::FlashOperationControllerTest test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "flash_operation_controller_test.moc"
```

Create `src/ui/desktop/flash/operation/BUILD.bazel`:

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "fastecu_qttest", "qt_cc_library")

package(default_visibility = ["//src/ui/desktop:__pkg__"])

# Runs one flash operation for MainWindow: Denso TCU service routing,
# portable workflow creation, the FlashDialog, and the unknown-protocol
# warning. It only passes SerialPortActions* through, so it needs no
# serial_qt_compat edge.
qt_cc_library(
    name = "flash_operation_controller",
    srcs = ["flash_operation_controller.cpp"],
    hdrs = ["flash_operation_controller.h"],
    copts = COMMON_COPTS,
    deps = QT_DEPS + [
        "//src/algorithms/protocol",
        "//src/backend/config:config_paths",
        "//src/backend/flash:flash_operation_request",
        "//src/backend/flash:flash_types",
        "//src/platform/desktop/common/flash:flash_worker",
        "//src/ui/desktop/flash/common:flash_dialog",
        "//src/ui/desktop/service_functions:denso_tcu_read_preflight",
    ],
)

fastecu_qttest(
    name = "flash_operation_controller_test",
    size = "small",
    src = "flash_operation_controller_test.cpp",
    copts = ["-DQT_WIDGETS_LIB"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [
        ":flash_operation_controller",
        "//src/platform/desktop/common/serial/testing:fake_serial_backend",
    ],
)
```

If a visibility error names a dependency, check that dependency package's `default_visibility`. The ones listed admit `//src/ui:__subpackages__` or `//src:__subpackages__`. For `//src/backend/config:config_paths`, if it doesn't, add `"//src/ui/desktop/flash/operation:__pkg__"` to that target's own visibility. That is not a ratchet list.

- [ ] **Step 2: Run it and confirm it fails**

Run: `bazel test --config=release //src/ui/desktop/flash/operation:flash_operation_controller_test`
Expected: FAIL: `flash_operation_controller.h` / `.cpp` missing.

- [ ] **Step 3: Implement the controller**

Create `src/ui/desktop/flash/operation/flash_operation_controller.h`:

```cpp
#pragma once

#include <QObject>
#include <QString>

#include <optional>
#include <string>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_types.h"

class QWidget;
class SerialPortActions;

namespace fastecu::flash
{

struct FlashOperationInput
{
    FlashOperation operation;
    std::string protocol;
    std::string mcu;
    std::string kernel_path; // for the Denso TCU "Dump" log line
    std::optional<bytes::Bytes> image;
    config::ConfigPaths paths;
    std::string display_filename;
};

enum class FlashOperationStatus
{
    Completed,            // a workflow ran; read_bytes/rom_id as the dialog returned them
    ServiceActionHandled, // a Denso TCU service action consumed the request
    Unsupported,          // no workflow for this protocol; warning shown
};

struct FlashOperationOutcome
{
    FlashOperationStatus status;
    std::optional<bytes::Bytes> read_bytes;
    std::optional<std::string> rom_id;
};

// Runs one flash operation for MainWindow and reports what happened. Owns no
// calibration state; MainWindow applies the outcome.
class FlashOperationController : public QObject
{
    Q_OBJECT

  public:
    FlashOperationController(SerialPortActions& serial, QWidget *dialog_parent);

    FlashOperationOutcome run(const FlashOperationInput& input);

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
    void external_logger(QString message);
    void external_logger(int value);

  private:
    SerialPortActions& serial_;
    QWidget *dialog_parent_;
};

} // namespace fastecu::flash
```

Create `src/ui/desktop/flash/operation/flash_operation_controller.cpp`:

```cpp
#include "src/ui/desktop/flash/operation/flash_operation_controller.h"

#include <QMessageBox>

#include <utility>

#include "src/backend/flash/flash_operation_request.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"
#include "src/ui/desktop/flash/common/flash_dialog.h"
#include "src/ui/desktop/service_functions/denso_tcu_read_preflight.h"

namespace fastecu::flash
{

FlashOperationController::FlashOperationController(SerialPortActions& serial, QWidget *dialog_parent)
    : serial_(serial), dialog_parent_(dialog_parent)
{
}

FlashOperationOutcome FlashOperationController::run(const FlashOperationInput& input)
{
    if (input.operation == FlashOperation::Read && is_denso_tcu_protocol(input.protocol))
    {
        using fastecu::service_functions::DensoTcuReadAction;
        const DensoTcuReadAction action = fastecu::service_functions::choose_denso_tcu_read_action(dialog_parent_);
        switch (action)
        {
        case DensoTcuReadAction::Dump:
            emit LOG_I("Read memory with flashmethod '" + QString::fromStdString(input.protocol) + "' and kernel '" +
                           QString::fromStdString(input.kernel_path) + "'",
                       true, true);
            break;
        case DensoTcuReadAction::Relearn:
            emit LOG_I("Attempting TCU relearn", true, true);
            break;
        case DensoTcuReadAction::ReadParameters:
            emit LOG_I("Attempting to read TCU parameters", true, true);
            break;
        case DensoTcuReadAction::SetParameters:
            emit LOG_I("Attempting to set TCU parameters", true, true);
            break;
        case DensoTcuReadAction::Cancelled:
            emit LOG_I("No option selected", true, true);
            break;
        }
        if (fastecu::service_functions::run_denso_tcu_service_action(action, &serial_, input.protocol, dialog_parent_))
        {
            return {.status = FlashOperationStatus::ServiceActionHandled};
        }
    }

    auto workflow = FlashWorkflowFactory::tryCreate({
        .operation = input.operation,
        .protocol = input.protocol,
        .mcu = input.mcu,
        .image = input.image,
        .paths = input.paths,
        .display_filename = input.display_filename,
        .serial = &serial_,
    });
    if (!workflow)
    {
        QMessageBox::warning(dialog_parent_, tr("Unknown flashmethod"),
                             "Unknown flashmethod! Flashmethod \"" + QString::fromStdString(input.protocol) +
                                 "\" not yet implemented!");
        return {.status = FlashOperationStatus::Unsupported};
    }

    FlashDialog flash_module(std::move(workflow), input.operation, QString::fromStdString(input.display_filename),
                             dialog_parent_);
    QObject::connect<void (FlashDialog::*)(QString)>(&flash_module, &FlashDialog::external_logger, this,
                                                     qOverload<QString>(&FlashOperationController::external_logger));
    QObject::connect<void (FlashDialog::*)(int)>(&flash_module, &FlashDialog::external_logger, this,
                                                 qOverload<int>(&FlashOperationController::external_logger));
    QObject::connect(&flash_module, &FlashDialog::LOG_E, this, &FlashOperationController::LOG_E);
    QObject::connect(&flash_module, &FlashDialog::LOG_W, this, &FlashOperationController::LOG_W);
    QObject::connect(&flash_module, &FlashDialog::LOG_I, this, &FlashOperationController::LOG_I);
    QObject::connect(&flash_module, &FlashDialog::LOG_D, this, &FlashOperationController::LOG_D);

    FlashDialogResult result = flash_module.run();
    return {
        .status = FlashOperationStatus::Completed,
        .read_bytes = std::move(result.accepted_read_bytes),
        .rom_id = std::move(result.rom_id),
    };
}

} // namespace fastecu::flash
```

If `FlashDialog`'s signal declarations differ from these connect forms, copy the exact `QObject::connect` template forms `MainWindow::start_ecu_operations` uses today (it connects the same four `LOG_*` signals and two `external_logger` overloads).

- [ ] **Step 4: Run it and confirm it passes**

Run: `bazel test --config=release //src/ui/desktop/flash/operation:flash_operation_controller_test --test_output=errors`
Expected: PASS (2 test functions, no Google Mock failures).

- [ ] **Step 5: Write the failing read-slot tests**

In `src/ui/desktop/mainwindow_test.cpp`:

1. Add this case before `private:`:

```cpp
    // Spec behavior change 2: a read that produces no calibration releases
    // the slot it allocated instead of leaking it.
    void readOfAnUnsupportedProtocolReleasesTheReadSlot()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        MainWindow window{services.services()};
        constructor_driver.stop();

        window.serial_ports = {"OpenPort 2.0"};
        window.serial_port_list->clear();
        window.serial_port_list->addItem("OpenPort 2.0");
        window.serial_port_list->setCurrentIndex(0);
        window.configValues->flash_protocol_selected_make = "Subaru";
        window.configValues->flash_protocol_selected_protocol_name = "sub_ecu_not_a_real_protocol";
        window.configValues->flash_protocol_selected_mcu = "SH7058";
        window.configValues->flash_protocol_selected_id = "0";
        window.configValues->flash_protocol_kernel = {"test-kernel.bin"};
        window.configValues->flash_protocol_kernel_addr = {"0xFFFF3000"};
        window.configValues->kernel_files_directory = config_root_.path() + "/kernels/";
        const int slot = window.ecuCalDefIndex;

        ModalDriver operation_driver{QString()};
        operation_driver.start();
        QCOMPARE(startEcuOperations(window, "read"), 0);
        operation_driver.stop();

        QVERIFY(!operation_driver.timedOut());
        QCOMPARE(window.ecuCalDefIndex, slot);
        QVERIFY(window.ecuCalDef[slot] == nullptr);
    }
```

2. In `handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling`, directly after `QVERIFY(!window.vbatt_timer->isActive());`, add:

```cpp
        QVERIFY(window.ecuCalDef[window.ecuCalDefIndex] == nullptr);
```

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow --test_output=errors`
Expected: FAIL: both `ecuCalDef[...] == nullptr` checks fail, because the slot is still allocated.

- [ ] **Step 6: Switch `MainWindow` to the controller and release the slot**

Add `"//src/ui/desktop/flash/operation:flash_operation_controller",` to `:desktop`'s `deps`. In `src/ui/desktop/mainwindow.cpp`, add `#include "src/ui/desktop/flash/operation/flash_operation_controller.h"` after the `denso_tcu_read_preflight.h` include, and delete the `denso_tcu_read_preflight.h` include if nothing else in the file uses it (`grep -n "service_functions::" src/ui/desktop/mainwindow.cpp`).

In `start_ecu_operations`, make these edits:

1. Directly after `        QByteArray fullRomDataTmp;` add:

```cpp
        // The read branch allocates the next calibration slot before dispatch
        // (update_protocol_info reads it); anything but a successful read
        // releases it again.
        bool release_read_slot = false;
```

2. In the read branch (`else` of `if (cmd_type == "test_write" || cmd_type == "write")`), directly after `ecuCalDef[rom_number] = new FileActions::EcuCalDefStructure;` add `            release_read_slot = true;`.

3. Replace everything from `        emit LOG_D("Protocol to use: "` through the closing `}` of the `if (cmd_type == "read") { … } else { … }` block (the last statement inside the make check) with:

```cpp
        emit LOG_D("Protocol to use: " + configValues->flash_protocol_selected_protocol_name, true, true);

        const fastecu::flash::FlashOperation operation =
            fastecu::flash::flash_operation_from_command(cmd_type.toStdString());

        fastecu::flash::FlashOperationController controller{*serial, this};
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::LOG_E, syslogger,
                         &SystemLogger::log_messages);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::LOG_W, syslogger,
                         &SystemLogger::log_messages);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::LOG_I, syslogger,
                         &SystemLogger::log_messages);
        QObject::connect(&controller, &fastecu::flash::FlashOperationController::LOG_D, syslogger,
                         &SystemLogger::log_messages);
        QObject::connect(&controller, qOverload<QString>(&fastecu::flash::FlashOperationController::external_logger),
                         this, &MainWindow::external_logger);
        QObject::connect(&controller, qOverload<int>(&fastecu::flash::FlashOperationController::external_logger),
                         this, &MainWindow::external_logger_set_progressbar_value);

        const fastecu::flash::FlashOperationOutcome outcome = controller.run({
            .operation = operation,
            .protocol = configValues->flash_protocol_selected_protocol_name.toStdString(),
            .mcu = ecuCalDef[rom_number]->McuType.toStdString(),
            .kernel_path = ecuCalDef[rom_number]->Kernel.toStdString(),
            .image = fastecu::flash::portableImageForOperation(operation, bytes::view(ecuCalDef[rom_number]->FullRomData)),
            .paths = fastecu::config::paths_from_config_values(*configValues),
            .display_filename = ecuCalDef[rom_number]->FileName.toStdString(),
        });

        if (outcome.status == fastecu::flash::FlashOperationStatus::Completed)
        {
            if (outcome.read_bytes)
            {
                ecuCalDef[rom_number]->FullRomData = bytes::toQByteArray(bytes::ByteView(*outcome.read_bytes));
            }
            if (outcome.rom_id)
            {
                ecuCalDef[rom_number]->RomId = QString::fromStdString(*outcome.rom_id);
            }
        }

        if (outcome.status == fastecu::flash::FlashOperationStatus::ServiceActionHandled)
        {
            // The old goto skipped the post-operation block entirely.
        }
        else if (cmd_type == "read")
        {
            if (ecuCalDef[ecuCalDefIndex]->FullRomData.length())
            {
                QDateTime dateTime = dateTime.currentDateTime();
                QString dateTimeString = dateTime.toString("yyyy-MM-dd_hh'h'mm'm'ss's'");

                ecuCalDef[ecuCalDefIndex]->FileName = QString::fromStdString(fastecu::flash::read_image_filename(
                    ecuCalDef[ecuCalDefIndex]->RomId.toStdString(), dateTimeString.toStdString()));

                fileActions->open_subaru_rom_file(ecuCalDef[ecuCalDefIndex], ecuCalDef[ecuCalDefIndex]->FileName);
                update_protocol_info(ecuCalDefIndex);
                if (!ecuCalDef[ecuCalDefIndex]->use_romraider_definition &&
                    !ecuCalDef[ecuCalDefIndex]->use_ecuflash_definition)
                {
                    prompt_for_missing_definition(ecuCalDef[ecuCalDefIndex]);
                }

                calibrationTreeWidget->buildCalibrationFilesTree(ecuCalDefIndex, ui->calibrationFilesTreeWidget,
                                                                 ecuCalDef[ecuCalDefIndex]);
                calibrationTreeWidget->buildCalibrationDataTree(ui->calibrationDataTreeWidget,
                                                                ecuCalDef[ecuCalDefIndex]);

                release_read_slot = false;
                ecuCalDefIndex++;
                save_calibration_file_as();
            }
        }
        else
        {
            ecuCalDef[rom_number]->FullRomData = fullRomDataTmp;
        }

        if (release_read_slot)
        {
            delete ecuCalDef[rom_number];
            ecuCalDef[rom_number] = nullptr;
        }
    }
    return 0;
}
```

This replaces the old `eeprom_paths`, `portable_image`, `protocol`, `denso_tcu`, TCU `switch`, `FlashWorkflowFactory::tryCreate`, `FlashDialog`, and "Unknown flashmethod" code, all of which now lives in the controller. `release_read_slot = false;` comes before `ecuCalDefIndex++` so the slot kept by a successful read is never freed.

- [ ] **Step 7: Drop `flash_dialog.h` from `mainwindow.h`**

Delete `#include "src/ui/desktop/flash/common/flash_dialog.h"` from `src/ui/desktop/mainwindow.h`. Run `bazel build --config=release //:fastecu //src/ui/...`. For each compile error naming a type the header used to bring in, add the specific header to the file that uses the type:
- `bytes::` → `src/algorithms/protocol/bytes.h`
- `fastecu::flash::FlashOperation` → `src/backend/flash/flash_types.h`
- `portableImageForOperation` → `src/platform/desktop/common/flash/flash_workflow.h`

Do not re-add `flash_dialog.h`.

Run: `grep -rn "flash_dialog.h" src/ui/desktop/*.h src/ui/desktop/*.cpp`
Expected: no matches.

- [ ] **Step 8: Run everything**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //... --test_output=errors`
Expected: PASS. That includes both read-slot checks, the unchanged Denso TCU counts (3 and 2), and the portable-route, future-suffix and cleanup cases.

- [ ] **Step 9: Measure**

Run: `awk '/^int MainWindow::start_ecu_operations/,/^}/' src/ui/desktop/mainwindow.cpp | wc -l`
Expected: roughly 160 or fewer. The write preflight and calibration handoff stay by design, so the spec's "roughly 80" was optimistic. Record the actual number in the PR description.

- [ ] **Step 10: Commit**

```bash
git add src/ui/desktop
git commit -m "refactor(ui): FlashOperationController runs flash dispatch for MainWindow (step 6d-3)"
```

- [ ] **Step 11: Gate**

Run `prek run --all-files`, `bazel test --config=release //...`, `bazel run //:clang_tidy_report_changed`; all must pass.

---

## PR 6d-4

```bash
git switch -c docs/step6d-4-flash-dispatch-closeout   # from the 6d-3 branch
```

### Task 5: Close out step 6d in the docs, and stack the PRs

**Files:**
- Modify: `docs/modularization-plan.md`, `docs/tech-debt.md`, `docs/design-notes.md`
- Delete: `docs/superpowers/specs/2026-09-26-step6d-flash-dispatch-design.md`, `docs/superpowers/plans/2026-09-26-step6d-flash-dispatch.md`

**Interfaces:**
- Consumes: the merged work of Tasks 1–4.
- Produces: nothing.

- [ ] **Step 1: Modularization plan**

In `docs/modularization-plan.md`:
- In the Status paragraph, change "and 6c (desktop composition root) are complete" to "6c (desktop composition root), and 6d (flash-operation dispatch) are complete".
- In step 6's list, directly after the 6c bullet (the one whose last line links the tech-debt roadmap) and before the platform-selection bullet, insert:

```markdown
   - **6d flash-operation dispatch — complete.** `MainWindow::start_ecu_operations`
     no longer routes flash operations. Pure decisions live in the portable
     `//src/backend/flash:flash_operation_request`, the idle-line serial reset
     in `//src/platform/desktop/common/serial:serial_idle`, and Denso TCU
     routing, workflow creation, and the `FlashDialog` in
     `//src/ui/desktop/flash/operation:flash_operation_controller`.
     `mainwindow.h` no longer includes `flash_dialog.h`. A scope guard
     replaced the `goto`, which also made the write-preflight early returns
     stop battery polling, and a read that produces no calibration now
     releases its slot. `test_mainwindow` now fails on Google Mock
     violations; before 6d it had 14 silent ones. See the
     [design notes](design-notes.md#flash-operation-dispatch).
```

- [ ] **Step 2: Tech-debt roadmap**

In `docs/tech-debt.md`, under "P1: Separate UI from application logic":
- Delete the bullet beginning "- Remove the `goto ecu_operation_cleanup;` in" through the end of that bullet.
- Replace the bullet "- Replace direct construction of all flash dialogs from `MainWindow` with a typed operation registry/factory that owns module-specific dependencies." with:

```markdown
- Flash dispatch runs through `FlashOperationController` (step 6d); what
  remains in `MainWindow::start_ecu_operations` is write preflight,
  checksum correction, and the post-read calibration handoff, which all
  mutate `ecuCalDef` and move with the "Replace parallel-list data models"
  work rather than on their own.
```

- Update the "`mainwindow.cpp` is about 2.5k lines" figure to the current `wc -l < src/ui/desktop/mainwindow.cpp` result, rounded to one decimal of a thousand.

- [ ] **Step 3: Design notes**

Add before `## Testing` in `docs/design-notes.md`:

```markdown
## Flash-operation dispatch

### `start_ecu_operations` cleanup is a scope guard

The idle-line serial reset, battery-polling stop, and log-transport
re-selection run from a `qScopeGuard` constructed right after the port check.
Every exit from that point runs them. Before step 6d the Denso TCU service
path reached them through a `goto`, and the write-preflight early returns
("No file selected!", Cancel on the checksum warning) skipped them, leaving
battery polling running.

### The read slot is allocated early and released on failure

The read path still allocates `ecuCalDef[ecuCalDefIndex]` before dispatch,
because `update_protocol_info` reads it. Anything but a successful read with
data (cancel, failure, `Unsupported`, a handled TCU service action) deletes
it and resets the slot to `nullptr`, its state before the first read.

### `reset_serial_to_idle` lives beside the facade

It calls `SerialPortActions` methods, so it needs the facade's definition,
and `serial_qt_compat`'s frozen visibility list could not gain the UI
package that calls it. It lives in the serial package as `serial_idle`,
visible only to `//src/ui/desktop`. `FlashOperationController` only passes
the facade pointer through, so it needs no serial edge at all.
```

In the `## Testing` section of `docs/design-notes.md`, add:

```markdown
### QtTest suites using Google Mock must fail on its failures

QtTest's exit status ignores Google Mock. A suite that uses it calls
`::testing::InitGoogleMock(&argc, argv)` in `main` and returns non-zero when
`::testing::Test::HasFailure()`. `test_mainwindow` lacked this until step
6d and passed with 14 violated expectations, all of them expectations that
had never matched the real call sequence.
```

- [ ] **Step 4: Delete the spec and plan, check links, commit**

```bash
git rm docs/superpowers/specs/2026-09-26-step6d-flash-dispatch-design.md docs/superpowers/plans/2026-09-26-step6d-flash-dispatch.md
prek run --all-files
git add docs
git commit -m "docs: close out step 6d (flash-operation dispatch)"
```

Expected: `prek` passes (the lychee link check confirms no link points at the deleted files).

- [ ] **Step 5: Stack and open the PRs (when authorized)**

```bash
gh stack init --base master test/step6d-1-enforce-gmock-and-helpers refactor/step6d-2-serial-idle-cleanup refactor/step6d-3-flash-operation-controller docs/step6d-4-flash-dispatch-closeout
gh stack submit --auto --open
```

Then edit each PR's title and body (`gh pr edit <n> --title … --body-file …`). Each body gets a summary, the test plan, and, for 6d-2 and 6d-3, the two behavior changes. 6d-3's body also records the `start_ecu_operations` line count from Task 4 Step 9.
