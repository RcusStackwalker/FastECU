#ifndef FAKE_BACKEND_H
#define FAKE_BACKEND_H

#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <functional>
#include <stdexcept>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Thrown by the throwNonStandardOn* controls below: deliberately NOT derived
// from std::exception, so it only matches a bare `catch (...)` -- exercises
// the adapters' generic catch-all branches (distinct from their
// `catch (const std::exception&)` branches, which the standard throwOnRead
// -style runtime_error controls exercise).
struct FakeBackendNonStandardFailure
{
};

// Scripted backend for facade-level tests. Subclasses the direct backend so
// the 44 config accessors come for free (real fields, no port ever opened);
// overrides the I/O entry points with scripted, observable behavior.
class FakeBackend : public SerialPortActionsDirect
{
    Q_OBJECT
  public:
    int readDelayMs = 0; // set before use; simulates a slow adapter
    QByteArray scriptedResponse;
    QSemaphore *readEntered = nullptr;
    QSemaphore *continueRead = nullptr;
    bool throwOnRead = false;
    bool throwOnIsOpen = false;
    std::atomic<bool> portOpen{true};
    std::function<void()> afterRead;
    bool *destroyed = nullptr;

    // -- opt-in controls added for adapter-level coverage; every one of them
    // defaults to a no-op so pre-existing tests that never touch these
    // fields keep observing the original behavior. --
    bool throwNonStandardOnRead = false;
    bool throwOnWrite = false;
    bool throwNonStandardOnWrite = false;
    bool closePortAfterRead = false;        // set portOpen=false right after a successful read
    bool closePortAfterWrite = false;       // set portOpen=false right after a successful write
    std::function<void()> beforeReadThrow;  // fires immediately before a scripted read failure
    std::function<void()> beforeWriteThrow; // fires immediately before a scripted write failure
    std::function<void()> beforeBaudThrow;  // fires immediately before a scripted baud-change failure

    // change_port_speed() result/exception controls. Default mirrors the
    // real backend's success code (STATUS_SUCCESS == 0) since no
    // pre-existing test exercises baud change through FakeBackend.
    int baudChangeResult = STATUS_SUCCESS;
    bool throwOnBaudChange = false;
    bool throwNonStandardOnBaudChange = false;
    bool closePortAfterBaud = false; // set portOpen=false after a non-throwing baud-change result

    // -- config-setter result controls for the desktop K-Line/CAN flash
    // transport adapter tests (step 5c, Task 12): every setter below
    // unconditionally `return true`s in the real backend
    // (serial_port_actions_direct.h) -- none of them touch the port or
    // hardware, they just assign a member. Each override still delegates
    // to SerialPortActionsDirect's own implementation first so the
    // underlying member -- and therefore the paired getter -- keeps
    // working exactly as before for every pre-existing test (e.g.
    // test_facade_threading.cpp's getSet_marshalsToBackendThread(),
    // test_flash_utils.cpp's configureIso15765Can_* tests); only the
    // *returned* bool is swapped for the controllable field below. Each
    // field defaults to that always-succeeds behavior; set one to false to
    // force the corresponding setter to fail and prove an adapter's
    // configure() stops at exactly that step. Every call is logged (via
    // the shared call log) as "cfg:<setter name>:<value>" so a test can
    // assert both which setters ran and in what order.
    bool isIso14230ConnectionResult = true;
    bool isCanConnectionResult = true;
    bool isIso15765ConnectionResult = true;
    bool is29BitIdResult = true;
    bool serialPortBaudrateResult = true;
    bool canSpeedResult = true;
    bool canSourceAddressResult = true;
    bool canDestinationAddressResult = true;
    bool iso15765SourceAddressResult = true;
    bool iso15765DestinationAddressResult = true;

    bool set_is_iso14230_connection(bool value) override
    {
        log(QString("cfg:set_is_iso14230_connection:%1").arg(value ? "1" : "0"));
        SerialPortActionsDirect::set_is_iso14230_connection(value);
        return isIso14230ConnectionResult;
    }
    bool set_is_can_connection(bool value) override
    {
        log(QString("cfg:set_is_can_connection:%1").arg(value ? "1" : "0"));
        SerialPortActionsDirect::set_is_can_connection(value);
        return isCanConnectionResult;
    }
    bool set_is_iso15765_connection(bool value) override
    {
        log(QString("cfg:set_is_iso15765_connection:%1").arg(value ? "1" : "0"));
        SerialPortActionsDirect::set_is_iso15765_connection(value);
        return isIso15765ConnectionResult;
    }
    bool set_is_29_bit_id(bool value) override
    {
        log(QString("cfg:set_is_29_bit_id:%1").arg(value ? "1" : "0"));
        SerialPortActionsDirect::set_is_29_bit_id(value);
        return is29BitIdResult;
    }
    bool set_serial_port_baudrate(QString value) override
    {
        log("cfg:set_serial_port_baudrate:" + value);
        SerialPortActionsDirect::set_serial_port_baudrate(value);
        return serialPortBaudrateResult;
    }
    bool set_can_speed(QString value) override
    {
        log("cfg:set_can_speed:" + value);
        SerialPortActionsDirect::set_can_speed(value);
        return canSpeedResult;
    }
    bool set_can_source_address(uint32_t value) override
    {
        log(QString("cfg:set_can_source_address:%1").arg(value));
        SerialPortActionsDirect::set_can_source_address(value);
        return canSourceAddressResult;
    }
    bool set_can_destination_address(uint32_t value) override
    {
        log(QString("cfg:set_can_destination_address:%1").arg(value));
        SerialPortActionsDirect::set_can_destination_address(value);
        return canDestinationAddressResult;
    }
    bool set_iso15765_source_address(uint32_t value) override
    {
        log(QString("cfg:set_iso15765_source_address:%1").arg(value));
        SerialPortActionsDirect::set_iso15765_source_address(value);
        return iso15765SourceAddressResult;
    }
    bool set_iso15765_destination_address(uint32_t value) override
    {
        log(QString("cfg:set_iso15765_destination_address:%1").arg(value));
        SerialPortActionsDirect::set_iso15765_destination_address(value);
        return iso15765DestinationAddressResult;
    }

    // open_serial_port() success/failure control (step 5c, Task 12): the
    // pre-existing hardcoded override just below this used to unconditionally
    // return an empty QString (every pre-existing test that touches
    // FakeBackend relies on that -- see the comment there), so the default
    // of an empty string preserves it exactly. Set to a non-empty string to
    // simulate a successful open.
    QString openSerialPortResult;

    ~FakeBackend() override
    {
        if (destroyed)
        {
            *destroyed = true;
        }
    }

    QStringList takeCallLog()
    {
        QMutexLocker l(&logMutex);
        QStringList out = callLog;
        callLog.clear();
        return out;
    }

    bool is_serial_port_open() override
    {
        if (throwOnIsOpen)
        {
            throw std::runtime_error("scripted backend open-state failure");
        }
        return portOpen.load();
    }

    // Real open_serial_port() unconditionally indexes serial_port_list.at(0)
    // (see serial_port_actions_direct.cpp) before doing any real hardware
    // probing -- fine for production where a port is always selected first,
    // but a test double has no port list populated. Stub it out like the
    // other I/O entry points below rather than touch real hardware/J2534.
    QString open_serial_port() override
    {
        log("open_serial_port");
        return openSerialPortResult;
    }

    QByteArray read_serial_data(uint16_t timeout) override
    {
        log(QString("read:begin:t=%1").arg(timeout));
        if (readEntered)
        {
            readEntered->release();
        }
        if (continueRead)
        {
            continueRead->acquire();
        }
        if (readDelayMs)
        {
            QThread::msleep(readDelayMs);
        }
        if (throwOnRead || throwNonStandardOnRead)
        {
            if (beforeReadThrow)
            {
                beforeReadThrow();
            }
            if (throwNonStandardOnRead)
            {
                throw FakeBackendNonStandardFailure{};
            }
            throw std::runtime_error("scripted backend read failure");
        }
        if (afterRead)
        {
            afterRead();
        }
        if (closePortAfterRead)
        {
            portOpen.store(false);
        }
        log("read:end");
        return scriptedResponse;
    }

    QByteArray write_serial_data(QByteArray output) override
    {
        return writeImpl("write", output);
    }

    // Distinct virtual from write_serial_data() in the backend interface --
    // the real implementation has its own body that touches the real
    // QSerialPort directly (byte-at-a-time write with echo readback), not a
    // wrapper around write_serial_data(). Callers that use the echo-check
    // variant (e.g. FlashEcuMitsuM32rCanOperation) would otherwise fall
    // through to that real-hardware path and hang/warn on an unopened port.
    QByteArray write_serial_data_echo_check(QByteArray output) override
    {
        return writeImpl("write_echo_check", output);
    }

    // Overrides the real (hardware-touching) change_port_speed() with a
    // scripted result; no pre-existing test calls it, so this is a pure
    // addition rather than a behavior change.
    int change_port_speed(QString portSpeed) override
    {
        log("baud:begin:" + portSpeed);
        if (throwOnBaudChange || throwNonStandardOnBaudChange)
        {
            if (beforeBaudThrow)
            {
                beforeBaudThrow();
            }
            if (throwNonStandardOnBaudChange)
            {
                throw FakeBackendNonStandardFailure{};
            }
            throw std::runtime_error("scripted backend baud-change failure");
        }
        if (closePortAfterBaud)
        {
            portOpen.store(false);
        }
        log("baud:end");
        return baudChangeResult;
    }

  private:
    // Shared by write_serial_data() and write_serial_data_echo_check(): both
    // production write paths (K-Line's plain write, CAN/SSM's echo-check
    // write) route through here so a single set of controls governs "write"
    // behavior regardless of which adapter is under test.
    QByteArray writeImpl(const char *label, const QByteArray& output)
    {
        log(QString(label) + ":begin:" + QString::fromLatin1(output.toHex()));
        if (readDelayMs)
        {
            QThread::msleep(readDelayMs);
        }
        if (throwOnWrite || throwNonStandardOnWrite)
        {
            if (beforeWriteThrow)
            {
                beforeWriteThrow();
            }
            if (throwNonStandardOnWrite)
            {
                throw FakeBackendNonStandardFailure{};
            }
            throw std::runtime_error("scripted backend write failure");
        }
        if (closePortAfterWrite)
        {
            portOpen.store(false);
        }
        log(QString(label) + ":end");
        return QByteArray(); // matches the real backend's empty return
    }

    void log(const QString& s)
    {
        QMutexLocker l(&logMutex);
        callLog.append(s);
    }
    QMutex logMutex;
    QStringList callLog;
};

#endif // FAKE_BACKEND_H
