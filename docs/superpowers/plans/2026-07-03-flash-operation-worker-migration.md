# Flash Operation Worker Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the 29 flash/eeprom `QDialog` modules' blocking serial I/O off the GUI thread onto a dedicated worker thread per operation, then delete the GUI compat shim spec 1a left in `SerialPortActions` for them.

**Architecture:** Each module's protocol logic (currently private `QDialog` methods) relocates verbatim into a new `XxxOperation : public FlashOperationWorker(QThread)` class. The existing dialog shrinks to GUI-only concerns (`setupUi`, `show()`/`close()`, `QMessageBox`, `ui->progressbar`) and creates/starts its `Operation` from `run()`, blocking on a local `QEventLoop` (not a `processEvents()` spin) until the worker signals `operationFinished`. This preserves `run()`'s existing blocking contract, so `mainwindow.cpp`'s ~30 stack-allocated dispatch call sites need no changes.

**Tech Stack:** Qt 6 (QThread, QEventLoop, QMetaObject::invokeMethod with Qt::BlockingQueuedConnection), qmake (`FastECU.pro`, `tests/tests.pro`), QTest.

**Spec:** `docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md`

## Global Constraints

- No protocol-behavior change: every `QMessageBox` prompt (wording, buttons, default), every I/O sequence, every timeout value stays identical. Relocation only.
- `run()` keeps blocking its caller until the operation is fully finished (success/failure `QMessageBox` shown, dialog closed on success) — do not convert to fire-and-forget async.
- `mainwindow.cpp` is not modified by this plan. If any task appears to require a `mainwindow.cpp` change, stop and re-check the design (`docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md`) — that would mean a deviation from the approved design.
- Only the 29 modules with a `void run()` method migrate (see Task list). `dtc_operations`, `dataterminal`, `hexcommander`, and `modules/biu/biu_operations_subaru` (interactive, click-driven — no `run()`/`kill_process`/progress bar) are explicitly out of scope.
- The compat shim in `serial_port/serial_port_actions.cpp` is deleted only after all 29 modules are migrated (Task 34, last).
- `FlashOperationWorker` is `#include`d and linked wherever a module and its tests need it — always add new files to **both** `FastECU.pro` and `tests/tests.pro` in the same task, since `tests.pro` already links the full `SerialPortActions` facade and `QtWidgets` (see `docs/logging-engine-tech-debt.md`'s note on `tests.pro`'s link surface).

---

## Task 1: `FlashOperationWorker` base class

**Files:**
- Create: `modules/flash_operation_worker.h`
- Create: `modules/flash_operation_worker.cpp`
- Test: `tests/test_flash_operation_worker.cpp`
- Test: `tests/test_flash_operation_worker.h`
- Modify: `FastECU.pro`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`

**Interfaces:**
- Produces: `FlashOperationWorker` (in `modules/flash_operation_worker.h`), a `QThread` subclass every subsequent task's `XxxOperation` class derives from. Public/protected surface used by later tasks:
  - `using PromptFn = std::function<int(QWidget *dialog, QString title, QString text, int buttons, int defaultButton)>;`
  - `explicit FlashOperationWorker(QWidget *dialog, QObject *parent = nullptr, PromptFn promptOverride = {});`
  - `void requestStop();` / `bool stopRequested() const;`
  - Signals: `LOG_E/W/I/D(QString,bool,bool)`, `externalLoggerMessage(QString)`, `progressChanged(int)`, `operationFinished(bool success)`
  - `protected: virtual bool execute() = 0;` (subclasses implement this — the relocated protocol sequence)
  - `protected: int confirm(const QString &title, const QString &text, QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton);`
  - `protected: void delay(int timeout);` (msleep-based; replaces every module's `processEvents()`-pumping `delay()`)
  - `protected: QWidget *m_dialog;`

- [ ] **Step 1: Write `modules/flash_operation_worker.h`**

```cpp
#ifndef FLASH_OPERATION_WORKER_H
#define FLASH_OPERATION_WORKER_H

#include <QAtomicInteger>
#include <QMessageBox>
#include <QMetaObject>
#include <QThread>
#include <QWidget>
#include <functional>

// Worker-thread half of a flash/eeprom module's dialog+operation split (see
// docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md).
// Subclasses relocate a module's protocol methods here and implement
// execute(); run() wraps it and emits operationFinished(). confirm() lets
// worker-thread code show a QMessageBox on the GUI thread and block for the
// answer -- every module has at least one mid-operation confirmation prompt
// today, interleaved with blocking I/O in the same function.
class FlashOperationWorker : public QThread
{
    Q_OBJECT

public:
    // (dialog, title, text, buttons, defaultButton) -> chosen button.
    // Tests substitute this to auto-answer without a real modal; production
    // code uses the default, which shows a real QMessageBox on dialog's thread.
    using PromptFn = std::function<int(QWidget *dialog, QString title, QString text,
                                        int buttons, int defaultButton)>;

    explicit FlashOperationWorker(QWidget *dialog, QObject *parent = nullptr,
                                   PromptFn promptOverride = {});

    void requestStop();
    bool stopRequested() const;

signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);
    void externalLoggerMessage(QString message);
    void progressChanged(int value);
    void operationFinished(bool success);

protected:
    void run() final;
    virtual bool execute() = 0;

    // Shows a QMessageBox on the GUI thread and blocks this (worker) thread
    // until the user answers. Must only be called from this worker's own
    // thread -- calling it from m_dialog's thread deadlocks.
    int confirm(const QString &title, const QString &text,
                QMessageBox::StandardButtons buttons,
                QMessageBox::StandardButton defaultButton);

    void delay(int timeout);

    QWidget *m_dialog;

private:
    QAtomicInteger<bool> m_stopRequested{false};
    PromptFn m_promptOverride;
};

#endif // FLASH_OPERATION_WORKER_H
```

- [ ] **Step 2: Write `modules/flash_operation_worker.cpp`**

```cpp
#include "flash_operation_worker.h"

FlashOperationWorker::FlashOperationWorker(QWidget *dialog, QObject *parent, PromptFn promptOverride)
    : QThread(parent)
    , m_dialog(dialog)
    , m_promptOverride(std::move(promptOverride))
{
    if (!m_promptOverride)
    {
        m_promptOverride = [](QWidget *dialog, QString title, QString text,
                               int buttons, int defaultButton) {
            return static_cast<int>(QMessageBox::warning(dialog, title, text,
                                     QMessageBox::StandardButtons(buttons),
                                     QMessageBox::StandardButton(defaultButton)));
        };
    }
}

void FlashOperationWorker::requestStop()
{
    m_stopRequested.storeRelaxed(true);
}

bool FlashOperationWorker::stopRequested() const
{
    return m_stopRequested.loadRelaxed();
}

void FlashOperationWorker::run()
{
    bool success = execute();
    emit operationFinished(success);
}

int FlashOperationWorker::confirm(const QString &title, const QString &text,
                                   QMessageBox::StandardButtons buttons,
                                   QMessageBox::StandardButton defaultButton)
{
    int result = static_cast<int>(defaultButton);
    PromptFn prompt = m_promptOverride;
    QWidget *dialog = m_dialog;
    QMetaObject::invokeMethod(dialog, [prompt, dialog, title, text, buttons, defaultButton]() {
        return prompt(dialog, title, text, int(buttons), int(defaultButton));
    }, Qt::BlockingQueuedConnection, &result);
    return result;
}

void FlashOperationWorker::delay(int timeout)
{
    QThread::msleep(timeout);
}
```

- [ ] **Step 3: Write the failing test `tests/test_flash_operation_worker.h`**

```cpp
#ifndef TEST_FLASH_OPERATION_WORKER_H
#define TEST_FLASH_OPERATION_WORKER_H
int run_test_flash_operation_worker(int argc, char **argv);
#endif
```

- [ ] **Step 4: Write the failing test `tests/test_flash_operation_worker.cpp`**

```cpp
#include <QtTest>
#include <QSignalSpy>
#include <QWidget>
#include "modules/flash_operation_worker.h"
#include "test_flash_operation_worker.h"

class ScriptedOperation : public FlashOperationWorker
{
    Q_OBJECT
public:
    using FlashOperationWorker::FlashOperationWorker;

    bool succeedResult = true;
    bool checkStopBeforeReturning = false;
    int confirmResultSeen = -1;

protected:
    bool execute() override
    {
        if (checkStopBeforeReturning)
        {
            while (!stopRequested())
                QThread::msleep(5);
            return false;
        }
        emit LOG_I("scripted operation running", true, true);
        emit progressChanged(50);
        confirmResultSeen = confirm("title", "text",
                                     QMessageBox::Yes | QMessageBox::Cancel,
                                     QMessageBox::Cancel);
        return succeedResult;
    }
};

class TestFlashOperationWorker : public QObject
{
    Q_OBJECT
private slots:
    void run_emitsOperationFinished_withExecuteResult()
    {
        QWidget dialog;
        ScriptedOperation op(&dialog);
        op.succeedResult = true;
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QCOMPARE(finishedSpy.size(), 1);
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
        QVERIFY(op.wait(2000));
    }

    void requestStop_isNoticedByExecute()
    {
        QWidget dialog;
        ScriptedOperation op(&dialog);
        op.checkStopBeforeReturning = true;
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QTest::qWait(20);
        op.requestStop();
        QVERIFY(finishedSpy.wait(2000));
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        QVERIFY(op.wait(2000));
    }

    void confirm_runsPromptOnDialogThread_andReturnsItsAnswer()
    {
        QWidget dialog;
        QThread *callingThread = nullptr;
        auto prompt = [&callingThread](QWidget *, QString, QString, int, int) -> int {
            callingThread = QThread::currentThread();
            return QMessageBox::Yes;
        };
        ScriptedOperation op(&dialog, nullptr, prompt);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(op.confirmResultSeen, int(QMessageBox::Yes));
        QCOMPARE(callingThread, dialog.thread());
    }

    void logAndProgressSignals_arriveAtCallingThread()
    {
        QWidget dialog;
        ScriptedOperation op(&dialog);
        QSignalSpy logSpy(&op, &FlashOperationWorker::LOG_I);
        QSignalSpy progressSpy(&op, &FlashOperationWorker::progressChanged);

        op.start();
        QVERIFY(op.wait(2000));

        QTRY_COMPARE(logSpy.size(), 1);
        QCOMPARE(progressSpy.size(), 1);
        QCOMPARE(progressSpy.at(0).at(0).toInt(), 50);
    }
};

int run_test_flash_operation_worker(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    TestFlashOperationWorker t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_flash_operation_worker.moc"
```

- [ ] **Step 5: Wire into `tests/tests.pro`**

In the `SOURCES +=` block, add (alphabetically, near the top with the other `test_*.cpp` entries):
```
    test_flash_operation_worker.cpp \
    ../modules/flash_operation_worker.cpp \
```
In the `HEADERS +=` block, add:
```
    test_flash_operation_worker.h \
    ../modules/flash_operation_worker.h \
```

- [ ] **Step 6: Wire into `tests/main.cpp`**

Add near the other `run_test_*` calls:
```cpp
    status |= run_test_flash_operation_worker(argc, argv);
```

- [ ] **Step 7: Wire into `FastECU.pro`**

In `SOURCES +=`, insert `modules/flash_operation_worker.cpp \` alphabetically between the `modules/eeprom/...` and `modules/jtag/...` entries (after line containing `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.cpp \`, before `modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag.cpp \`).
In `HEADERS +=`, insert `modules/flash_operation_worker.h \` at the equivalent alphabetical position (after `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.h \`, before `modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag.h \`).

- [ ] **Step 8: Build and run the test**

Run: `cd tests && qmake6 tests.pro && make -j4 && ./mut_dma_tests`
Expected: build succeeds; `TestFlashOperationWorker`'s 4 test functions pass (look for `PASS   : TestFlashOperationWorker::...` lines, 0 failures overall).

- [ ] **Step 9: Commit**

```bash
git add modules/flash_operation_worker.h modules/flash_operation_worker.cpp \
        tests/test_flash_operation_worker.h tests/test_flash_operation_worker.cpp \
        tests/tests.pro tests/main.cpp FastECU.pro
git commit -m "feat: add FlashOperationWorker base class for flash-module thread separation"
```

---

## Task 2: `FlashEcuMitsuM32rCanOperation` (pilot module, part 1 — new Operation class)

**Files:**
- Create: `modules/ecu/flash_ecu_mitsu_m32r_can_operation.h`
- Create: `modules/ecu/flash_ecu_mitsu_m32r_can_operation.cpp`
- Reference (read, do not modify yet): `modules/ecu/flash_ecu_mitsu_m32r_can.h`, `modules/ecu/flash_ecu_mitsu_m32r_can.cpp`

**Interfaces:**
- Consumes: `FlashOperationWorker` from Task 1 (`modules/flash_operation_worker.h`).
- Produces: `FlashEcuMitsuM32rCanOperation`, constructed as
  `FlashEcuMitsuM32rCanOperation(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *dialog, QObject *parent = nullptr, PromptFn promptOverride = {})`.
  Used by Task 3 (the dialog) and Task 4 (its test).

This class holds the exact relocated body of `FlashEcuMitsuM32rCan`'s former `connect_bootloader()`, `read_mem()`, `write_mem()`, `upload_and_commit()`, `build_request()`, `parse_message_to_hex()`, plus the config-setup/MCU-lookup code that used to be inline at the top of `run()`. `set_progressbar_value(v)` calls become `emit progressChanged(v)`; `delay(ms)` calls now resolve to the base class's `QThread::msleep`-based `delay()`; `if (kill_process) return STATUS_ERROR;` becomes `if (stopRequested()) return STATUS_ERROR;`; the one mid-operation `QMessageBox::warning(this, ...)` (the "erase trigger" prompt in `write_mem()`) becomes `confirm(...)`.

- [ ] **Step 1: Write `modules/ecu/flash_ecu_mitsu_m32r_can_operation.h`**

```cpp
#ifndef FLASH_ECU_MITSU_M32R_CAN_OPERATION_H
#define FLASH_ECU_MITSU_M32R_CAN_OPERATION_H

#include <QByteArray>
#include <QString>

#include <file_actions.h>
#include "modules/flash_operation_worker.h"

class SerialPortActions;

// Worker-thread half of FlashEcuMitsuM32rCan (see
// docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md).
// Owns every serial-> call and the Mitsubishi Colt CZT (Z37A) M32R CAN
// bootloader protocol sequence; relocated verbatim from
// FlashEcuMitsuM32rCan's former private methods.
class FlashEcuMitsuM32rCanOperation : public FlashOperationWorker
{
    Q_OBJECT

public:
    FlashEcuMitsuM32rCanOperation(SerialPortActions *serial,
                                   FileActions::EcuCalDefStructure *ecuCalDef,
                                   QString cmd_type,
                                   QWidget *dialog,
                                   QObject *parent = nullptr,
                                   PromptFn promptOverride = {});

protected:
    bool execute() override;

private:
    #define STATUS_SUCCESS  0x00
    #define STATUS_ERROR    0x01

    int connect_bootloader();
    int read_mem(uint32_t start_addr, uint32_t length);
    int write_mem(bool test_write);
    bool upload_and_commit(uint32_t start, const QByteArray &data);
    QByteArray build_request(const QByteArray &sidPayload);
    QString parse_message_to_hex(QByteArray received);

    SerialPortActions *serial;
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;

    int mcu_type_index = 0;
    QString mcu_type_string;

    uint16_t serial_read_timeout = 500;
    uint16_t serial_read_extra_long_timeout = 3000;
};

#endif // FLASH_ECU_MITSU_M32R_CAN_OPERATION_H
```

- [ ] **Step 2: Write `modules/ecu/flash_ecu_mitsu_m32r_can_operation.cpp`**

```cpp
#include "flash_ecu_mitsu_m32r_can_operation.h"
#include "serial_port_actions.h"
#include "protocol/mitsu_colt_can_protocol.h"
#include <kernelmemorymodels.h>

FlashEcuMitsuM32rCanOperation::FlashEcuMitsuM32rCanOperation(
        SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef,
        QString cmd_type, QWidget *dialog, QObject *parent, PromptFn promptOverride)
    : FlashOperationWorker(dialog, parent, std::move(promptOverride))
    , serial(serial)
    , ecuCalDef(ecuCalDef)
    , cmd_type(cmd_type)
{
}

bool FlashEcuMitsuM32rCanOperation::execute()
{
    mcu_type_string = ecuCalDef->McuType;
    mcu_type_index = 0;
    while (flashdevices[mcu_type_index].name != 0)
    {
        if (flashdevices[mcu_type_index].name == mcu_type_string)
            break;
        mcu_type_index++;
    }
    emit LOG_D("MCU type: " + QString(flashdevices[mcu_type_index].name) + " index: " + QString::number(mcu_type_index), true, true);

    serial->set_is_iso14230_connection(false);
    serial->set_add_iso14230_header(false);
    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(true);
    serial->set_is_29_bit_id(false);
    serial->set_can_speed("500000");
    serial->set_iso15765_source_address(0x7E0);
    serial->set_iso15765_destination_address(0x7E8);
    serial->open_serial_port();

    emit LOG_I("Connecting to Mitsubishi Colt CZT M32R CAN bootloader, please wait...", true, true);
    int result = connect_bootloader();

    if (result == STATUS_SUCCESS)
    {
        if (cmd_type == "read")
        {
            emit externalLoggerMessage("Reading ROM, please wait...");
            emit LOG_I("Reading ROM from ECU using CAN", true, true);
            result = read_mem(flashdevices[mcu_type_index].fblocks[0].start, flashdevices[mcu_type_index].romsize);
        }
        else if (cmd_type == "write")
        {
            emit externalLoggerMessage("Writing ROM, please wait...");
            emit LOG_I("Writing ROM to ECU using CAN", true, true);
            result = write_mem(false);
        }
    }

    return result == STATUS_SUCCESS;
}

QByteArray FlashEcuMitsuM32rCanOperation::build_request(const QByteArray &sidPayload)
{
    QByteArray output;
    output.append(char(0x00));
    output.append(char(0x00));
    output.append(char(0x07));
    output.append(char(0xE0));
    output.append(sidPayload);
    return output;
}

int FlashEcuMitsuM32rCanOperation::connect_bootloader()
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;
    QString msg;

    if (!serial->is_serial_port_open())
    {
        emit LOG_I("ERROR: Serial port is not open.", true, true);
        return STATUS_ERROR;
    }

    bool needSecurity = (cmd_type == "write");
    quint8 sessionId = needSecurity ? kSessionBootload : kSessionBasic;

    emit LOG_I("Starting diagnostic session...", true, true);
    output = build_request(buildDiagnosticSessionFrame(sessionId));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 5 || (uint8_t)received.at(4) != (kServiceDiagnosticSession + 0x40) || (uint8_t)received.at(5) != sessionId)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Diagnostic session ok", true, true);

    if (!needSecurity)
        return STATUS_SUCCESS;

    emit LOG_I("Requesting security seed...", true, true);
    output = build_request(buildSecurityAccessSeedRequestFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 9 || (uint8_t)received.at(4) != (kServiceSecurityAccess + 0x40) || (uint8_t)received.at(5) != 5)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }

    QByteArray seed = received.mid(6, 4);
    msg = parse_message_to_hex(seed);
    emit LOG_I("Received seed: " + msg, true, true);

    QByteArray key = seedKey(seed);
    msg = parse_message_to_hex(key);
    emit LOG_I("Calculated seed key: " + msg, true, true);

    emit LOG_I("Sending seed key to ECU...", true, true);
    output = build_request(buildSecurityAccessKeyFrame(key));
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 5 || (uint8_t)received.at(4) != (kServiceSecurityAccess + 0x40) || (uint8_t)received.at(5) != 6)
    {
        emit LOG_E("Wrong response from ECU: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Security access ok", true, true);

    return STATUS_SUCCESS;
}

int FlashEcuMitsuM32rCanOperation::read_mem(uint32_t start_addr, uint32_t length)
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;
    QByteArray romdata;

    uint32_t addr = start_addr;
    uint32_t end_addr = start_addr + length;

    emit progressChanged(0);
    emit LOG_I("Start reading ROM, please wait...", true, true);

    while (addr < end_addr)
    {
        if (stopRequested())
            return STATUS_ERROR;

        output = build_request(buildReadMemoryByAddressFrame(addr, (quint8)kFlashReadBlockSize));
        serial->write_serial_data_echo_check(output);
        delay(50);
        received = serial->read_serial_data(serial_read_timeout);

        if (received.length() < int(4 + 1 + kFlashReadBlockSize) || (uint8_t)received.at(4) != (kServiceReadMemoryByAddress + 0x40))
        {
            emit LOG_E("Wrong response from ECU at 0x" + QString::number(addr, 16) + ": " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return STATUS_ERROR;
        }

        romdata.append(received.mid(5, int(kFlashReadBlockSize)));
        addr += kFlashReadBlockSize;

        float pleft = (float)(addr - start_addr) / (float)length * 100.0f;
        emit progressChanged((int)pleft);
    }

    emit LOG_I("ROM read complete", true, true);
    ecuCalDef->FullRomData = romdata;
    emit progressChanged(100);

    return STATUS_SUCCESS;
}

bool FlashEcuMitsuM32rCanOperation::upload_and_commit(uint32_t start, const QByteArray &data)
{
    using namespace MitsuColtCan;

    QByteArray output;
    QByteArray received;

    output = build_request(buildRequestDownloadFrame(start, data.size()));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestDownload + 0x40))
    {
        emit LOG_E("RequestDownload to 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    const QVector<QByteArray> chunks = buildTransferDataFrames(data);
    for (const QByteArray &chunk : chunks)
    {
        output = build_request(chunk);
        serial->write_serial_data_echo_check(output);
        delay(50);
        received = serial->read_serial_data(serial_read_timeout);
        if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceTransferData + 0x40))
        {
            emit LOG_E("TransferData to 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
            return false;
        }
    }

    output = build_request(buildRequestDownloadFrame(kCrcTransferAddress, kCrcTransferSize));
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestDownload + 0x40))
    {
        emit LOG_E("RequestDownload for checksum rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    quint16 crc = checksum(data);
    QByteArray crcData;
    crcData.append(char((crc >> 8) & 0xFF));
    crcData.append(char(crc & 0xFF));
    output = build_request(buildTransferDataFrames(crcData).first());
    serial->write_serial_data_echo_check(output);
    delay(50);
    received = serial->read_serial_data(serial_read_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceTransferData + 0x40))
    {
        emit LOG_E("TransferData for checksum rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    output = build_request(buildRoutineCheckCrcFrame(start));
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRoutineControl + 0x40))
    {
        emit LOG_E("RoutineControl CRC check for 0x" + QString::number(start, 16) + " rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return false;
    }

    return true;
}

int FlashEcuMitsuM32rCanOperation::write_mem(bool test_write)
{
    using namespace MitsuColtCan;
    Q_UNUSED(test_write);

    QByteArray romdata = ecuCalDef->FullRomData;
    if ((uint32_t)romdata.size() < kUserspaceEnd)
    {
        emit LOG_E("ROM file too small: need at least 0x" + QString::number(kUserspaceEnd, 16) + " bytes", true, true);
        return STATUS_ERROR;
    }

    emit LOG_I("Uploading erase-page routine to RAM 0x" + QString::number(kEraseRoutineRamAddr, 16) + "...", true, true);
    if (!upload_and_commit(kEraseRoutineRamAddr, QByteArray(reinterpret_cast<const char *>(kErasePageRoutine), sizeof(kErasePageRoutine))))
    {
        emit LOG_E("Erase-page routine upload failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Erase page uploaded", true, true);

    emit LOG_I("Uploading write-page routine to RAM 0x" + QString::number(kWriteRoutineRamAddr, 16) + "...", true, true);
    if (!upload_and_commit(kWriteRoutineRamAddr, QByteArray(reinterpret_cast<const char *>(kWritePageRoutine), sizeof(kWritePageRoutine))))
    {
        emit LOG_E("Write-page routine upload failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Write page uploaded", true, true);

    // --- HIGH RISK STEP ---
    // This 12-byte ServiceRequestReflash(0x3B) payload is carried over verbatim
    // from externals/livemonitor/obdsessionwidget.cpp:180-181, where the
    // original author's own comment reads "caused bootloader lockup" during
    // their testing. Only attempt this on a bench/spare ECU with a recovery
    // path available (see this project's boot-talk utility for bricked-ECU
    // recovery). See docs/superpowers/specs/2026-06-30-fastecu-colt-can-reflash-design.md
    // for full context.
    int reflashConfirm = confirm(tr("Erase trigger"),
        tr("About to send the flash-erase trigger command. This exact "
           "sequence is known to have locked up the bootloader during the "
           "original implementation's testing. Only continue if this is a "
           "bench/spare ECU with a recovery path available.\n\nContinue?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (reflashConfirm != QMessageBox::Yes)
    {
        emit LOG_I("Erase trigger canceled by user", true, true);
        return STATUS_ERROR;
    }

    QByteArray output = build_request(buildRequestReflashUnlockFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    QByteArray received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRequestReflash + 0x40))
    {
        emit LOG_E("Reflash unlock rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }

    output = build_request(buildRoutineEraseFrame());
    serial->write_serial_data_echo_check(output);
    delay(200);
    received = serial->read_serial_data(serial_read_extra_long_timeout);
    if (received.length() <= 4 || (uint8_t)received.at(4) != (kServiceRoutineControl + 0x40))
    {
        emit LOG_E("Erase trigger rejected: " + FileActions::parse_nrc_message(received.mid(4, received.length() - 1)), true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Userspace flash erased", true, true);

    emit LOG_I("Writing ROM userspace 0x" + QString::number(kUserspaceStart, 16) + "-0x" + QString::number(kUserspaceEnd, 16) + "...", true, true);
    QByteArray userspace = romdata.mid(int(kUserspaceStart), int(kUserspaceEnd - kUserspaceStart));
    if (!upload_and_commit(kUserspaceStart, userspace))
    {
        emit LOG_E("ROM userspace write failed", true, true);
        return STATUS_ERROR;
    }
    emit LOG_I("Userspace flash written", true, true);

    return STATUS_SUCCESS;
}

QString FlashEcuMitsuM32rCanOperation::parse_message_to_hex(QByteArray received)
{
    QString msg;
    for (int i = 0; i < received.length(); i++)
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    return msg;
}
```

- [ ] **Step 3: Do not build yet** — `flash_ecu_mitsu_m32r_can.cpp` still has the original private methods this duplicates, so the project won't compile cleanly until Task 3 removes them. Continue to Task 3 before building.

---

## Task 3: Shrink `FlashEcuMitsuM32rCan` to the GUI-only dialog (pilot module, part 2)

**Files:**
- Modify: `modules/ecu/flash_ecu_mitsu_m32r_can.h` (full rewrite)
- Modify: `modules/ecu/flash_ecu_mitsu_m32r_can.cpp` (full rewrite)
- Modify: `FastECU.pro`

**Interfaces:**
- Consumes: `FlashEcuMitsuM32rCanOperation` from Task 2, `FlashOperationWorker` from Task 1.
- Produces: `FlashEcuMitsuM32rCan`'s public shape (constructor, `run()`, `LOG_E/W/I/D`, `external_logger(QString)`/`external_logger(int)`) is **unchanged** — `mainwindow.cpp` line 1309 (`FlashEcuMitsuM32rCan flash_module(serial, ecuCalDef[rom_number], cmd_type, this); connect_signals_and_run_module(&flash_module);`) needs no edit.

**Note on behavior:** the MCU-type lookup, `serial->set_*` config calls, and `open_serial_port()` used to run *before* the "Turn ignition ON" `QMessageBox` in the old `run()`. They now run *after* it (inside the `Operation`, which only starts once the user clicks OK), since the design places all `serial->` calls on the worker thread. This is a deliberate, minor reordering — flagged here and in the design's risk section, not a silent behavior change.

- [ ] **Step 1: Rewrite `modules/ecu/flash_ecu_mitsu_m32r_can.h`**

```cpp
#ifndef FLASH_ECU_MITSU_M32R_CAN_H
#define FLASH_ECU_MITSU_M32R_CAN_H

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QMainWindow>
#include <QMessageBox>
#include <QSerialPort>
#include <QTime>
#include <QTimer>
#include <QWidget>

#include <kernelmemorymodels.h>
#include <file_actions.h>
#include <ui_ecu_operations.h>

class SerialPortActions;
class FlashEcuMitsuM32rCanOperation;

QT_BEGIN_NAMESPACE
namespace Ui
{
    class EcuOperationsWindow;
}
QT_END_NAMESPACE

// Mitsubishi Colt CZT (Z37A, ROM 47110032) CAN reflash module. GUI-thread
// half of the dialog+operation split (see
// docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md);
// protocol logic lives in FlashEcuMitsuM32rCanOperation.
class FlashEcuMitsuM32rCan : public QDialog
{
    Q_OBJECT

public:
    explicit FlashEcuMitsuM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent = nullptr);
    ~FlashEcuMitsuM32rCan();

    void run();

signals:
    void external_logger(QString message);
    void external_logger(int value);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

private:
    FileActions::EcuCalDefStructure *ecuCalDef;
    QString cmd_type;

    void closeEvent(QCloseEvent *event);
    void set_progressbar_value(int value);

    SerialPortActions *serial;
    Ui::EcuOperationsWindow *ui;
    FlashEcuMitsuM32rCanOperation *m_operation = nullptr;
};

#endif // FLASH_ECU_MITSU_M32R_CAN_H
```

- [ ] **Step 2: Rewrite `modules/ecu/flash_ecu_mitsu_m32r_can.cpp`**

```cpp
#include "flash_ecu_mitsu_m32r_can.h"
#include "flash_ecu_mitsu_m32r_can_operation.h"
#include "serial_port_actions.h"

FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::EcuOperationsWindow)
    , ecuCalDef(ecuCalDef)
    , cmd_type(cmd_type)
{
    ui->setupUi(this);

    if (cmd_type == "write")
        this->setWindowTitle("Write ROM " + ecuCalDef->FileName + " to ECU");
    else if (cmd_type == "read")
        this->setWindowTitle("Read ROM from ECU");

    this->serial = serial;
}

FlashEcuMitsuM32rCan::~FlashEcuMitsuM32rCan()
{
    delete ui;
}

void FlashEcuMitsuM32rCan::closeEvent(QCloseEvent *event)
{
    if (m_operation)
        m_operation->requestStop();
}

void FlashEcuMitsuM32rCan::run()
{
    this->show();
    set_progressbar_value(0);

    int ret = QMessageBox::warning(this, tr("Connecting to ECU"),
                                   tr("Turn ignition ON and press OK to start initializing connection to ECU"),
                                   QMessageBox::Ok | QMessageBox::Cancel,
                                   QMessageBox::Ok);

    switch (ret)
    {
        case QMessageBox::Ok:
        {
            m_operation = new FlashEcuMitsuM32rCanOperation(serial, ecuCalDef, cmd_type, this);
            connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashEcuMitsuM32rCan::LOG_E);
            connect(m_operation, &FlashOperationWorker::LOG_W, this, &FlashEcuMitsuM32rCan::LOG_W);
            connect(m_operation, &FlashOperationWorker::LOG_I, this, &FlashEcuMitsuM32rCan::LOG_I);
            connect(m_operation, &FlashOperationWorker::LOG_D, this, &FlashEcuMitsuM32rCan::LOG_D);
            connect(m_operation, &FlashOperationWorker::externalLoggerMessage,
                    this, [this](QString msg) { emit external_logger(msg); });
            connect(m_operation, &FlashOperationWorker::progressChanged,
                    this, &FlashEcuMitsuM32rCan::set_progressbar_value);

            QEventLoop loop;
            bool success = false;
            connect(m_operation, &FlashOperationWorker::operationFinished, &loop,
                    [&success, &loop](bool ok) { success = ok; loop.quit(); });

            m_operation->start();
            loop.exec();
            m_operation->wait();
            delete m_operation;
            m_operation = nullptr;

            emit external_logger("Finished");

            if (success)
            {
                QMessageBox::information(this, tr("ECU Operation"), "ECU operation was succesful, press OK to exit");
                this->close();
            }
            else
            {
                QMessageBox::warning(this, tr("ECU Operation"), "ECU operation failed, press OK to exit and try again");
            }
            break;
        }
        case QMessageBox::Cancel:
            emit LOG_D("Operation canceled", true, true);
            this->close();
            break;
        default:
            QMessageBox::warning(this, tr("Connecting to ECU"), "Unknown operation selected!");
            emit LOG_D("Unknown operation selected!", true, true);
            this->close();
            break;
    }
}

void FlashEcuMitsuM32rCan::set_progressbar_value(int value)
{
    bool valueChanged = true;
    if (ui->progressbar)
    {
        valueChanged = ui->progressbar->value() != value;
        ui->progressbar->setValue(value);
    }
    if (valueChanged)
        emit external_logger(value);
}
```

- [ ] **Step 3: Wire the new Operation files into `FastECU.pro`**

In `SOURCES +=`, insert `modules/ecu/flash_ecu_mitsu_m32r_can_operation.cpp \` immediately after the existing `modules/ecu/flash_ecu_mitsu_m32r_can.cpp \` line.
In `HEADERS +=`, insert `modules/ecu/flash_ecu_mitsu_m32r_can_operation.h \` immediately after the existing `modules/ecu/flash_ecu_mitsu_m32r_can.h \` line.

- [ ] **Step 4: Build the full application**

Run: `qmake6 FastECU.pro && make -j4`
Expected: builds cleanly with no errors. (Warnings about the pre-existing `#define STATUS_SUCCESS`/`STATUS_ERROR` macro redefinition across module headers are expected and pre-existing — verify none are *new* errors, only the same warning class already present before this change.)

- [ ] **Step 5: Commit**

```bash
git add modules/ecu/flash_ecu_mitsu_m32r_can.h modules/ecu/flash_ecu_mitsu_m32r_can.cpp \
        modules/ecu/flash_ecu_mitsu_m32r_can_operation.h modules/ecu/flash_ecu_mitsu_m32r_can_operation.cpp \
        FastECU.pro
git commit -m "refactor: move FlashEcuMitsuM32rCan's protocol I/O onto a worker thread"
```

---

## Task 4: Operation-level test for the pilot module

**Files:**
- Create: `tests/test_flash_ecu_mitsu_m32r_can_operation.h`
- Create: `tests/test_flash_ecu_mitsu_m32r_can_operation.cpp`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`

**Interfaces:**
- Consumes: `FlashEcuMitsuM32rCanOperation` (Task 2), `FakeBackend` (`tests/fake_backend.h`, existing), `SerialPortActions`'s `backendFactoryForTests` constructor seam (existing, spec 1a).

**Scope note:** this tests the *relocated orchestration logic* (error propagation, `stopRequested()` interrupting the read loop) using minimal scripted responses — not full protocol correctness (already covered by the existing `test_mitsu_colt_can_protocol.cpp`) and not a full-ROM read/write (would require thousands of loop iterations against `flashdevices[]`'s real ROM sizes — too slow for a unit test; that path is bench-gated, per the design's Testing section).

- [ ] **Step 1: Write the failing test `tests/test_flash_ecu_mitsu_m32r_can_operation.h`**

```cpp
#ifndef TEST_FLASH_ECU_MITSU_M32R_CAN_OPERATION_H
#define TEST_FLASH_ECU_MITSU_M32R_CAN_OPERATION_H
int run_test_flash_ecu_mitsu_m32r_can_operation(int argc, char **argv);
#endif
```

- [ ] **Step 2: Write the failing test `tests/test_flash_ecu_mitsu_m32r_can_operation.cpp`**

```cpp
#include <QtTest>
#include <QSignalSpy>
#include <QWidget>
#include "modules/ecu/flash_ecu_mitsu_m32r_can_operation.h"
#include "serial_port_actions.h"
#include "fake_backend.h"
#include "file_actions.h"
#include "test_flash_ecu_mitsu_m32r_can_operation.h"

class TestFlashEcuMitsuM32rCanOperation : public QObject
{
    Q_OBJECT
private slots:
    void connectFailure_wrongResponse_returnsFalseWithoutLooping()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(false);   // create backend
        fake->scriptedResponse = QByteArray();   // too short: connect_bootloader() rejects it

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_128KB";
        QWidget dialog;
        FlashEcuMitsuM32rCanOperation op(&serial, &ecuCalDef, "read", &dialog);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);
        QSignalSpy errSpy(&op, &FlashOperationWorker::LOG_E);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        QVERIFY2(errSpy.size() >= 1, "expected an LOG_E about the wrong ECU response");
    }

    void requestStop_duringReadLoop_stopsWithoutFinishingRead()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(false);
        // Valid diagnostic-session response for the "read" (basic) session:
        // build_request()'s header (00 00 07 E0) + service 0x50 + session 0x81.
        fake->scriptedResponse = QByteArray("\x00\x00\x07\xE0\x50\x81", 6);

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_128KB";
        QWidget dialog;
        FlashEcuMitsuM32rCanOperation op(&serial, &ecuCalDef, "read", &dialog);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        op.requestStop();   // requested immediately: at most one read-loop iteration runs
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        QVERIFY(ecuCalDef.FullRomData.isEmpty());   // read never completed
    }
};

int run_test_flash_ecu_mitsu_m32r_can_operation(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    TestFlashEcuMitsuM32rCanOperation t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_flash_ecu_mitsu_m32r_can_operation.moc"
```

- [ ] **Step 3: Wire into `tests/tests.pro`**

`SOURCES +=`: add `test_flash_ecu_mitsu_m32r_can_operation.cpp \` and `../modules/ecu/flash_ecu_mitsu_m32r_can_operation.cpp \` near the other `test_*` entries.
`HEADERS +=`: add `test_flash_ecu_mitsu_m32r_can_operation.h \` and `../modules/ecu/flash_ecu_mitsu_m32r_can_operation.h \`.

(`../modules/ecu/flash_ecu_mitsu_m32r_can.cpp`/`.h` are **not** added here — `tests.pro` doesn't link the full dialog, only the Operation, since the dialog is pure GUI with nothing left to unit-test.)

- [ ] **Step 4: Wire into `tests/main.cpp`**

```cpp
    status |= run_test_flash_ecu_mitsu_m32r_can_operation(argc, argv);
```

- [ ] **Step 5: Build and run**

Run: `cd tests && qmake6 tests.pro && make -j4 && ./mut_dma_tests`
Expected: both `TestFlashEcuMitsuM32rCanOperation` test functions pass.

- [ ] **Step 6: Commit**

```bash
git add tests/test_flash_ecu_mitsu_m32r_can_operation.h tests/test_flash_ecu_mitsu_m32r_can_operation.cpp \
        tests/tests.pro tests/main.cpp
git commit -m "test: cover FlashEcuMitsuM32rCanOperation's error and stop-request paths"
```

---

## Task 5: Bench-checklist note for the pilot

**Files:**
- Modify: `docs/logging-engine-tech-debt.md` (or the project's active bench-checklist doc if a more specific one exists — check for `docs/*bench-checklist*.md` before editing)

**Interfaces:** none (documentation only).

- [ ] **Step 1: Check for an existing flash-module bench checklist**

Run: `ls docs/*bench-checklist* docs/*flash* 2>/dev/null`

- [ ] **Step 2: Add a note recording the pattern-proof status**

If no dedicated flash-module bench checklist exists yet, add a new section to `docs/logging-engine-tech-debt.md` (or create `docs/flash-operation-worker-bench-checklist.md` if one of the remaining-module tasks below expects a dedicated file — decide based on Step 1's findings) recording:

```markdown
## Flash operation worker migration (phase 1b)

`FlashEcuMitsuM32rCan` is the pattern-proof for the flash-module worker-thread
migration (see `docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md`).
Needs real-hardware re-verification before its next live flash: the QEventLoop-based
run() must correctly service the mid-operation confirm() prompt (the "erase trigger"
warning in write_mem()) while the worker thread is blocked waiting for it, and the
progress bar must update live during a real multi-minute read/write.
```

- [ ] **Step 3: Commit**

```bash
git add docs/logging-engine-tech-debt.md
git commit -m "docs: flag FlashEcuMitsuM32rCan for bench re-verification after worker migration"
```

---

## Migration Recipe (reference for Tasks 6-33)

Every remaining module follows the exact same transformation applied in Tasks 2-3. This
section is the complete, precise procedure each of Tasks 6-33 applies to its own module —
read it once, then follow it literally for each file pair.

**Inputs for module `Xxx`** (dialog class `FlashEcuXxx`/`FlashTcuXxx`/`EepromEcuXxx`, files
`modules/<category>/xxx.h` / `.cpp`):

1. **Read the current `modules/<category>/xxx.h` and `modules/<category>/xxx.cpp` in full.**
2. **Create `modules/<category>/xxx_operation.h`:**
   - Class `XxxOperation : public FlashOperationWorker`, `Q_OBJECT`.
   - Constructor: `XxxOperation(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, QString cmd_type, QWidget *dialog, QObject *parent = nullptr, PromptFn promptOverride = {})` — matches every module's existing dialog constructor parameters (`serial`, `ecuCalDef`, `cmd_type`) plus the base class's `dialog`/`parent`/`promptOverride`.
   - `protected: bool execute() override;`
   - Every **private method** from the original dialog header EXCEPT `closeEvent`, `set_progressbar_value`, and `delay` moves here unchanged (same name, same signature, same visibility). `set_progressbar_value` and `delay` are dropped entirely — replaced by the base class's `progressChanged` signal and `delay()`, respectively. `closeEvent` stays on the dialog.
   - Every **private data member** the moved methods use (protocol constants, timeouts, `mcu_type_string`/`mcu_type_index`-style state, `#define STATUS_SUCCESS`/`STATUS_ERROR` if present) moves here too. `serial`, `ecuCalDef`, `cmd_type` become this class's own members (set in the constructor), not passed through the dialog.
   - `kill_process` is **deleted** — replaced by the inherited `stopRequested()`.
3. **Create `modules/<category>/xxx_operation.cpp`:**
   - Constructor body: member-initializes `serial`, `ecuCalDef`, `cmd_type`, forwards `dialog`/`parent`/`promptOverride` to `FlashOperationWorker`'s constructor.
   - `execute()`: the relocated body of the original `run()`, **minus** `this->show()`, minus the very first pre-flight `QMessageBox` (if `run()`'s structure has one before any `serial->` call — most modules do, matching Mitsu CAN's "Turn ignition ON" prompt) and minus the final success/failure `QMessageBox`+`this->close()` block. Everything between those — MCU/model lookup, `serial->set_*` config calls, `serial->open_serial_port()`, the connect/read/write dispatch — becomes `execute()`'s body, ending in `return result == STATUS_SUCCESS;` (or the module's equivalent boolean).
   - Every relocated method's body is copied **verbatim** from the original `.cpp`, with these substitutions applied wherever they occur inside a relocated method:
     - `if (kill_process) return X;` -> `if (stopRequested()) return X;` (any early-return value `X` the original used).
     - `set_progressbar_value(N);` -> `emit progressChanged(N);`
     - `delay(N);` -> unchanged call syntax (`delay(N);`) — now resolves to the inherited `FlashOperationWorker::delay()` instead of the deleted local one.
     - `QMessageBox::warning(this, TITLE, TEXT, BUTTONS, DEFAULT)` (or `::information`/`::question` with the same shape) **occurring inside a relocated method** -> `confirm(TITLE, TEXT, BUTTONS, DEFAULT)` (drop the `this`/dialog argument; `confirm()` already knows the dialog).
     - `emit external_logger(QString ...)` **occurring inside a relocated method** -> `emit externalLoggerMessage(...)` (same argument).
     - `emit external_logger(int ...)` **occurring inside a relocated method**: should not occur — `int` progress values go through `set_progressbar_value`/`progressChanged`, not `external_logger(int)`, in every module inspected. If found, flag it rather than guessing — it means this module's structure differs from the pattern and needs a one-off decision.
     - `LOG_E`/`LOG_W`/`LOG_I`/`LOG_D` calls: unchanged (same signal names exist on `FlashOperationWorker`).
4. **Rewrite `modules/<category>/xxx.h`:** keep exactly `setupUi`-related members, `run()`, `closeEvent()`, `set_progressbar_value(int)`, the signal list, `ecuCalDef`/`cmd_type`/`serial`/`ui` members, and add `FlashEcuXxxOperation *m_operation = nullptr;` plus a forward declaration `class FlashEcuXxxOperation;` (or `FlashTcuXxxOperation`/`EepromEcuXxxOperation` matching the module's naming). Add `#include <QEventLoop>`. Remove the relocated methods' declarations and `kill_process`.
5. **Rewrite `modules/<category>/xxx.cpp`:** constructor and destructor unchanged. `closeEvent()` becomes `if (m_operation) m_operation->requestStop();`. `set_progressbar_value()` unchanged (still touches `ui->progressbar` and emits `external_logger(int)` — this stays exactly as today, it's the GUI-thread slot receiving `progressChanged`). `run()` follows the exact shape built in Task 3 Step 2 for `FlashEcuMitsuM32rCan::run()`: `this->show()`, any pre-flight `QMessageBox`/setup that stays on the GUI thread, then construct+wire+start the `XxxOperation` inside a `QEventLoop`, then the final result-handling `QMessageBox`+`close()`/`external_logger("Finished")` exactly as the original did after its own protocol call returned.
6. **Add both new files to `FastECU.pro`**, immediately adjacent (alphabetically) to the existing `modules/<category>/xxx.cpp`/`.h` entries, same style as Task 3 Step 3.
7. **Build the full application** (`qmake6 FastECU.pro && make -j4`) and confirm it compiles cleanly.
8. **Commit** both the new Operation files and the modified dialog files together, one commit per module.

Operation-level unit tests are **not required** for each of the remaining 28 modules — Task
1's base-class test already covers the shared `confirm()`/`stopRequested()`/signal-relay
mechanics generically, and per the design's Testing section, deep per-protocol scripted
tests are reserved for modules with the most distinctive logic. A build pass plus bench
verification is the gate for the remaining 28. If a future contributor wants to add one for
a specific module, follow Task 4's shape (fake `SerialPortActions` backend, scripted
responses, `QSignalSpy` on `operationFinished`).

---

## Task 6: Migrate `modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can`

**Files:**
- Create: `modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.h`
- Create: `modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp`
- Modify: `modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.h`
- Modify: `modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.cpp`
- Modify: `FastECU.pro`

**Interfaces:** `FlashEcuSubaruDenso1N83M_1_5MCanOperation`, same constructor shape as Task 2's `FlashEcuMitsuM32rCanOperation`. Dialog class `FlashEcuSubaruDenso1N83M_1_5MCan`'s public shape unchanged.

- [ ] **Step 1:** Apply the Migration Recipe above to `modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.{h,cpp}`, producing `FlashEcuSubaruDenso1N83M_1_5MCanOperation`.
- [ ] **Step 2:** Add the two new files to `FastECU.pro` (Recipe step 6).
- [ ] **Step 3:** Build: `qmake6 FastECU.pro && make -j4`. Expected: clean build.
- [ ] **Step 4: Commit**
```bash
git add modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.h modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.cpp \
        modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.h modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp \
        FastECU.pro
git commit -m "refactor: move FlashEcuSubaruDenso1N83M_1_5MCan's protocol I/O onto a worker thread"
```

---

## Task 7: Migrate `modules/ecu/flash_ecu_subaru_denso_1n83m_4m_can`

**Files:** same shape as Task 6, for `modules/ecu/flash_ecu_subaru_denso_1n83m_4m_can.{h,cpp}` -> `FlashEcuSubaruDenso1N83M_4MCanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit (same message pattern as Task 6, substituting the class name).

---

## Task 8: Migrate `modules/ecu/flash_ecu_subaru_denso_mc68hc16y5_02`

**Files:** `modules/ecu/flash_ecu_subaru_denso_mc68hc16y5_02.{h,cpp}` -> `FlashEcuSubaruDensoMC68HC16Y5_02Operation`. Header already read in this plan's research phase — confirmed same `run()`/`kill_process`/`closeEvent`/`set_progressbar_value`/`delay` shape as the pilot, plus additional relocated methods: `upload_kernel`, `init_flash_write`, `reflash_block`, `flash_block`, `get_changed_blocks`, `check_romcrc`, `init_crc32_tab`, `crc32`, `check_programming_voltage`, `request_kernel_init`, `request_kernel_id`, `add_ssm_header`, `calculate_checksum`, `check_received_message` (all move to the Operation per Recipe step 2/3).

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 9: Migrate `modules/ecu/flash_ecu_subaru_denso_sh7055_02`

**Files:** `modules/ecu/flash_ecu_subaru_denso_sh7055_02.{h,cpp}` -> `FlashEcuSubaruDensoSH7055_02Operation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 10: Migrate `modules/ecu/flash_ecu_subaru_denso_sh7058_can`

**Files:** `modules/ecu/flash_ecu_subaru_denso_sh7058_can.{h,cpp}` -> `FlashEcuSubaruDensoSH7058CanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 11: Migrate `modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel`

**Files:** `modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel.{h,cpp}` -> `FlashEcuSubaruDensoSH7058CanDieselOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 12: Migrate `modules/ecu/flash_ecu_subaru_denso_sh705x_densocan`

**Files:** `modules/ecu/flash_ecu_subaru_denso_sh705x_densocan.{h,cpp}` -> `FlashEcuSubaruDensoSH705xDensoCanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 13: Migrate `modules/ecu/flash_ecu_subaru_denso_sh705x_kline`

**Files:** `modules/ecu/flash_ecu_subaru_denso_sh705x_kline.{h,cpp}` -> `FlashEcuSubaruDensoSH705xKlineOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 14: Migrate `modules/ecu/flash_ecu_subaru_denso_sh72531_can`

**Files:** `modules/ecu/flash_ecu_subaru_denso_sh72531_can.{h,cpp}` -> `FlashEcuSubaruDensoSH72531CanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 15: Migrate `modules/ecu/flash_ecu_subaru_denso_sh72543_can_diesel`

**Files:** `modules/ecu/flash_ecu_subaru_denso_sh72543_can_diesel.{h,cpp}` -> `FlashEcuSubaruDensoSH72543CanDieselOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 16: Migrate `modules/ecu/flash_ecu_subaru_hitachi_m32r_can`

**Files:** `modules/ecu/flash_ecu_subaru_hitachi_m32r_can.{h,cpp}` -> `FlashEcuSubaruHitachiM32rCanOperation`. Note: this header's constructor omits `= nullptr` on `parent` (confirmed during research) — keep that same signature (no default) on both the dialog and preserve it when passing through to the Operation's constructor default (`QWidget *dialog` in the Operation itself still defaults are irrelevant since the dialog always passes `this`).

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 17: Migrate `modules/ecu/flash_ecu_subaru_hitachi_m32r_kline`

**Files:** `modules/ecu/flash_ecu_subaru_hitachi_m32r_kline.{h,cpp}` -> `FlashEcuSubaruHitachiM32rKlineOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 18: Migrate `modules/ecu/flash_ecu_subaru_hitachi_sh7058_can`

**Files:** `modules/ecu/flash_ecu_subaru_hitachi_sh7058_can.{h,cpp}` -> `FlashEcuSubaruHitachiSH7058CanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 19: Migrate `modules/ecu/flash_ecu_subaru_hitachi_sh72543r_can`

**Files:** `modules/ecu/flash_ecu_subaru_hitachi_sh72543r_can.{h,cpp}` -> `FlashEcuSubaruHitachiSH72543rCanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 20: Migrate `modules/ecu/flash_ecu_subaru_mitsu_m32r_kline`

**Files:** `modules/ecu/flash_ecu_subaru_mitsu_m32r_kline.{h,cpp}` -> `FlashEcuSubaruMitsuM32rKlineOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 21: Migrate `modules/ecu/flash_ecu_subaru_unisia_jecs_m32r`

**Files:** `modules/ecu/flash_ecu_subaru_unisia_jecs_m32r.{h,cpp}` -> `FlashEcuSubaruUnisiaJecsM32rOperation`. Note: constructor omits `= nullptr` default on `parent`, same as Task 16 — preserve as-is.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 22: Migrate `modules/ecu/flash_ecu_subaru_unisia_jecs`

**Files:** `modules/ecu/flash_ecu_subaru_unisia_jecs.{h,cpp}` -> `FlashEcuSubaruUnisiaJecsOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 23: Migrate `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can`

**Files:** `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can.{h,cpp}` -> `EepromEcuSubaruDensoSH705xCanOperation`. Note: constructor is not marked `explicit` in the original (confirmed during research) — preserve that (harmless either way, minimize diff).

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 24: Migrate `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline`

**Files:** `modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.{h,cpp}` -> `EepromEcuSubaruDensoSH705xKlineOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 25: Migrate `modules/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm`

**Files:** `modules/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm.{h,cpp}` -> `FlashEcuSubaruDensoMC68HC16Y5_02_BDMOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 26: Migrate `modules/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode`

**Files:** `modules/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode.{h,cpp}` -> `FlashEcuSubaruUnisiaJecsM32rBootModeOperation`. Note: constructor omits `= nullptr` default on `parent`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 27: Migrate `modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag`

**Files:** `modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag.{h,cpp}` -> `FlashEcuSubaruHitachiM32rJtagOperation`. Header already read in this plan's research phase — confirmed relocated methods include the JTAG bit-banging helpers (`init_jtag`, `hard_reset_jtag`, `read_idcode`, `read_usercode`, `set_rtdenb`, `read_tool_rom_code`, `write_jtag_ir`, `read_jtag_dr`, `write_jtag_dr`, `read_response`, `add_header`, `calculate_checksum`) alongside `read_mem`/`write_mem` — all move to the Operation per Recipe step 2/3, same as any other relocated private method (they still ultimately go through `serial->` calls).

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 28: Migrate `modules/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can`

**Files:** `modules/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can.{h,cpp}` -> `FlashTcuCvtSubaruHitachiM32rCanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 29: Migrate `modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can`

**Files:** `modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can.{h,cpp}` -> `FlashTcuCvtSubaruMitsuMH8104CanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 30: Migrate `modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can`

**Files:** `modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can.{h,cpp}` -> `FlashTcuCvtSubaruMitsuMH8111CanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 31: Migrate `modules/tcu/flash_tcu_subaru_denso_sh705x_can`

**Files:** `modules/tcu/flash_tcu_subaru_denso_sh705x_can.{h,cpp}` -> `FlashTcuSubaruDensoSH705xCanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 32: Migrate `modules/tcu/flash_tcu_subaru_hitachi_m32r_can`

**Files:** `modules/tcu/flash_tcu_subaru_hitachi_m32r_can.{h,cpp}` -> `FlashTcuSubaruHitachiM32rCanOperation`.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.

---

## Task 33: Migrate `modules/tcu/flash_tcu_subaru_hitachi_m32r_kline`

**Files:** `modules/tcu/flash_tcu_subaru_hitachi_m32r_kline.{h,cpp}` -> `FlashTcuSubaruHitachiM32rKlineOperation`. This is the last of the 29 modules.

- [ ] **Step 1:** Apply the Migration Recipe.
- [ ] **Step 2:** Add to `FastECU.pro`.
- [ ] **Step 3:** Build and verify clean.
- [ ] **Step 4:** Commit.
- [ ] **Step 5: Verify no module still references `kill_process` or the old pumping `delay()`/`set_progressbar_value()` pattern in a relocated context**

Run: `grep -rl "kill_process" modules/*/*.h modules/*/*.cpp`
Expected: no output (every module's `kill_process` has been replaced by `stopRequested()`/deleted).

---

## Task 34: Delete the GUI compat shim

**Files:**
- Modify: `serial_port/serial_port_actions.cpp`
- Modify: `tests/test_facade_threading.cpp` (remove the now-obsolete shim test)
- Modify: `tests/serial_backend_tests.pro` (no source list change expected, just re-run to confirm)
- Modify: `docs/logging-engine-tech-debt.md`

**Interfaces:** `SerialPortActions::waitForDone()`'s signature and callers are unchanged — only its internal branching is simplified.

- [ ] **Step 1: Simplify `waitForDone()` in `serial_port/serial_port_actions.cpp`**

Replace:
```cpp
void SerialPortActions::waitForDone(const std::shared_ptr<QSemaphore> &done)
{
    if (qApp && QThread::currentThread() == qApp->thread())
    {
        // COMPAT SHIM — deleted by spec 1b (flash-op worker migration).
        // Flash modules still run on the GUI thread; pumping here keeps the
        // progress UI alive during multi-minute operations, exactly like the
        // old in-backend pumps did. Reentrant facade calls dispatched by this
        // pump queue behind the in-flight one on the I/O thread instead of
        // interleaving on the wire.
        while (!done->tryAcquire())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
    else
    {
        done->acquire();
    }
}
```
with:
```cpp
void SerialPortActions::waitForDone(const std::shared_ptr<QSemaphore> &done)
{
    // No GUI-thread pump: every caller either runs on its own worker thread
    // (flash-module operations, LoggingWorker) or accepts a brief blocking
    // wait for a short, click-bounded call (dtc_operations, dataterminal,
    // hexcommander). See docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md.
    done->acquire();
}
```

- [ ] **Step 2: Remove the obsolete shim test from `tests/test_facade_threading.cpp`**

Delete the `guiThreadCaller_compatShim_keepsEventLoopAlive()` test function (its whole body,
lines ~138-159) and its declaration in the `private slots:` list (line ~22). This test
specifically asserted the pump kept a `QTimer` firing during a blocking GUI-thread call —
behavior this task intentionally removes.

- [ ] **Step 3: Build and run the serial backend test suite**

Run: `cd tests && qmake6 serial_backend_tests.pro && make -j4 && ./serial_backend_tests`
Expected: all remaining `TestFacadeThreading` tests pass (the shim test is gone, not failing).

- [ ] **Step 4: Update `docs/logging-engine-tech-debt.md`**

Find the paragraph ending "...backends now live on a dedicated `SerialIoThread` behind a
marshaling facade, and `processEvents()` remains only as a compat shim in the facade's
GUI-thread `waitForDone()`, pending spec 1b's flash-op worker migration." and replace its
final clause with: "...backends now live on a dedicated `SerialIoThread` behind a marshaling
facade; the GUI-thread compat shim was removed once the flash-op worker migration landed
(spec `2026-07-03-flash-operation-worker-design.md`) — `waitForDone()` now blocks
unconditionally regardless of caller thread."

- [ ] **Step 5: Commit**

```bash
git add serial_port/serial_port_actions.cpp tests/test_facade_threading.cpp docs/logging-engine-tech-debt.md
git commit -m "refactor: delete the GUI-thread compat shim now all flash modules use worker threads"
```

---

## Task 35: Full regression run

**Files:** none modified — verification only.

- [ ] **Step 1: Build and run every existing test suite**

```bash
cd tests
qmake6 tests.pro && make -j4 && ./mut_dma_tests
qmake6 serial_backend_tests.pro && make -j4 && ./serial_backend_tests
qmake6 serial_crash_tests.pro && make -j4 && ./serial_crash_tests
qmake6 mut_dma_integration_tests.pro && make -j4 && ./mut_dma_integration_tests
```
Expected: zero failures across all four binaries.

- [ ] **Step 2: Build the full application**

Run: `qmake6 FastECU.pro && make -j4`
Expected: clean build, no errors.

- [ ] **Step 3: Manually smoke-check a non-migrated GUI-thread consumer**

Since Task 34 changed `waitForDone()`'s behavior for every caller (not just the 29 migrated
modules), launch the app and open the Data Terminal or Hex Commander dialog, send one message,
and confirm the UI responds normally (a brief pause during the send is expected and accepted
per the design's scope decision — confirm it's brief, not a multi-second freeze or a hang).

- [ ] **Step 4: Update the design spec's status line**

In `docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md`, change:
```
**Status:** Approved for planning
```
to:
```
**Status:** Landed — pending bench re-verification per module (see docs/logging-engine-tech-debt.md)
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md
git commit -m "docs: record flash operation worker migration landing"
```
