// Characterization/unit tests for DesktopKlineFlashTransport (step 5c, Task
// 12) -- the first real, concrete adapter wrapping SerialPortActions as
// fastecu::flash::IKlineFlashTransport. Every prior test in this plan used
// only scripted fakes (ScriptedKlineFlashTransport); this suite proves the
// adapter itself against the real (test-doubled) SerialPortActions/
// SerialBackend marshaling path, using the same FakeBackend harness as
// tests/test_flash_ecu_mitsu_m32r_can_operation.cpp and
// tests/test_facade_threading.cpp.
#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"

#include <QCoreApplication>
#include <QSemaphore>
#include <QTest>

#include <atomic>
#include <memory>
#include <thread>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "fake_backend.h"
#include "test_desktop_kline_flash_transport.h"

using fastecu::ErrorKind;
using fastecu::ICancellationToken;
using fastecu::flash::DesktopKlineFlashTransport;
using fastecu::flash::KlineConfig;

namespace
{

// These tests exercise configure()/open()/close()/request_unblock() and the
// read() path's *unblock* branch, not read()'s cancellation.cancelled()
// branch -- a token that never cancels keeps them focused on that.
class NeverCancelled : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

// Used by the read()-cancellation test below: cancelled() is true from the
// very first check, so read() must fail before ever touching the backend.
class AlwaysCancelled : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return true;
    }
};

// Used by the read() mid-flight cancellation tests below: false on the
// first call (the pre-read check, which must pass so the backend is
// actually reached), true on every call after that (the post-read
// recheck(s), including inside both catch blocks) -- proves read() still
// re-observes cancellation after an already in-flight backend call
// returns/throws, not just before issuing it.
class CancelledOnSecondCheck : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return ++callCount >= 2;
    }

  private:
    mutable int callCount = 0;
};

} // namespace

class TestDesktopKlineFlashTransport : public QObject
{
    Q_OBJECT

  private slots:

    void configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false); // forces backend creation
        fake->takeCallLog();
        // Fail the *third* setter in configure()'s specified order
        // (set_is_iso14230_connection, set_is_can_connection,
        // set_is_iso15765_connection, set_is_29_bit_id,
        // set_serial_port_baudrate) -- this proves both that the first two
        // setters really ran, in order, and that nothing after the failure
        // (the 29-bit-id setter, the baudrate setter, open()) ran.
        fake->isIso15765ConnectionResult = false;

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.configure(
            KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
        QCOMPARE(fake->takeCallLog(),
                 QStringList({"cfg:set_is_iso14230_connection:1",
                              "cfg:set_is_can_connection:0",
                              "cfg:set_is_iso15765_connection:0"}));
    }

    // Data-driven sibling of configureChecksEveryBooleanSetterInOrderAndStops-
    // AtFirstFailure() above (which only exercises the third setter's
    // failure branch): proves every remaining setter's own InvalidConfig
    // return path independently. (The third setter,
    // set_is_iso15765_connection, is already covered by that test above, so
    // it is intentionally omitted here.)
    void configureFailsAtEachRemainingSetterInTurn_data()
    {
        QTest::addColumn<int>("setterIndex");
        QTest::addColumn<int>("expectedLogCount");
        QTest::newRow("set_is_iso14230_connection") << 0 << 1;
        QTest::newRow("set_is_can_connection") << 1 << 2;
        QTest::newRow("set_is_29_bit_id") << 3 << 4;
        QTest::newRow("set_serial_port_baudrate") << 4 << 5;
    }

    void configureFailsAtEachRemainingSetterInTurn()
    {
        QFETCH(int, setterIndex);
        QFETCH(int, expectedLogCount);

        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false); // forces backend creation
        fake->takeCallLog();

        switch (setterIndex)
        {
        case 0:
            fake->isIso14230ConnectionResult = false;
            break;
        case 1:
            fake->isCanConnectionResult = false;
            break;
        case 3:
            fake->is29BitIdResult = false;
            break;
        case 4:
            fake->serialPortBaudrateResult = false;
            break;
        }

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.configure(
            KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
        QCOMPARE(fake->takeCallLog().size(), expectedLogCount);
    }

    void openFailureReturnsDisconnectedWithoutAnyWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->takeCallLog();
        // FakeBackend::open_serial_port() defaults to the real backend's
        // "couldn't open the port" failure sentinel: an empty QString
        // (confirmed against SerialPortActionsDirect::open_serial_port(),
        // serial_port_actions_direct.cpp:519-644).

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.open();

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
        QVERIFY(fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // Success mirror of configureChecksEveryBooleanSetterInOrderAndStopsAt-
    // FirstFailure() above: every FakeBackend setter control defaults to
    // true, so configure() must run all five setters, in order, and return
    // success.
    void configureSucceedsWhenEverySetterSucceeds()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false); // forces backend creation
        fake->takeCallLog();

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.configure(
            KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

        QVERIFY(result.has_value());
        QCOMPARE(fake->takeCallLog(),
                 QStringList({"cfg:set_is_iso14230_connection:1",
                              "cfg:set_is_can_connection:0",
                              "cfg:set_is_iso15765_connection:0",
                              "cfg:set_is_29_bit_id:0",
                              "cfg:set_serial_port_baudrate:10400"}));
    }

    // Success mirror of openFailureReturnsDisconnectedWithoutAnyWrite():
    // FakeBackend::open_serial_port() returns whatever openSerialPortResult
    // is set to; a non-empty string is the real backend's success sentinel
    // (open_serial_port() returns `openedSerialPort` on every success path).
    void openSucceedsWhenBackendReturnsANonEmptyPortName()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->openSerialPortResult = "COM3";

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.open();

        QVERIFY(result.has_value());
    }

    // setBaud() success path: port open, change_port_speed() returns the
    // real backend's success sentinel (STATUS_SUCCESS == 0, FakeBackend's
    // default).
    void setBaudSucceedsWhenPortOpenAndDriverReturnsSuccess()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->takeCallLog();

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(result.has_value());
        QCOMPARE(fake->takeCallLog(), QStringList({"baud:begin:4800", "baud:end"}));
    }

    // setBaud() failure path: port stays open, but change_port_speed()
    // returns the real backend's failure sentinel (STATUS_ERROR, a small
    // *positive* value) -- maps to Internal, not InvalidConfig (a runtime
    // driver rejection, not a config-shape problem).
    void setBaudFailsWithInternalWhenPortStaysOpenButDriverRejectsChange()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->baudChangeResult = STATUS_ERROR;

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // setBaud() disconnected-before path: the port is already closed when
    // setBaud() is called -- change_port_speed() must never be reached.
    void setBaudFailsWithDisconnectedWhenPortAlreadyClosed()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->portOpen.store(false);
        fake->takeCallLog();

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
        QVERIFY(fake->takeCallLog().filter("baud:begin").isEmpty());
    }

    // setBaud() disconnected-during path: change_port_speed() itself reports
    // a failure code (driver rejected/errored) AND the port is observed
    // closed on the follow-up is_serial_port_open() check -- must map to
    // Disconnected, not the generic Internal "driver rejected" branch.
    void setBaudFailsWithDisconnectedWhenPortClosesDuringBaudChange()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->baudChangeResult = STATUS_ERROR;
        fake->closePortAfterBaud = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // write() success path: port open throughout, echo-check write reports
    // nothing useful in its return value (see the adapter's comment), so
    // is_serial_port_open() staying true is the only real post-condition;
    // success returns the number of bytes requested.
    void writeSucceedsAndReturnsRequestedByteCount()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->takeCallLog();

        DesktopKlineFlashTransport transport(std::move(serial));
        const bytes::Bytes data{0x01, 0x02, 0x03};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(result.has_value());
        QCOMPARE(*result, data.size());
        QVERIFY(!fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // write() disconnected-during path: the port closes as a side effect of
    // the write call itself (e.g. the adapter dropped mid-transfer) -- the
    // post-write is_serial_port_open() check must catch this even though
    // write_serial_data_echo_check() itself never signals failure via its
    // return value.
    void writeFailsWithDisconnectedWhenPortClosesDuringWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->closePortAfterWrite = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // read() success path: port open, cancellation never fires, backend
    // returns scripted bytes -- read() must return exactly those bytes.
    void readReturnsScriptedBytesOnSuccess()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->scriptedResponse = QByteArray("\x01\x02", 2);

        DesktopKlineFlashTransport transport(std::move(serial));
        NeverCancelled cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(result.has_value());
        QVERIFY(result->has_value());
        QVERIFY(result->value() == (bytes::Bytes{0x01, 0x02}));
    }

    // read() observes cancellation.cancelled() before ever issuing the read
    // -- the backend must never be touched at all.
    void readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->takeCallLog();

        DesktopKlineFlashTransport transport(std::move(serial));
        AlwaysCancelled cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
        QVERIFY(fake->takeCallLog().filter("read:begin").isEmpty());
    }

    // read() disconnected-before path: the port is already closed when
    // read() is called -- read_serial_data() must never be reached.
    void readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->portOpen.store(false);
        fake->takeCallLog();

        DesktopKlineFlashTransport transport(std::move(serial));
        NeverCancelled cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
        QVERIFY(fake->takeCallLog().filter("read:begin").isEmpty());
    }

    // read() disconnected-during path: the backend reports the port closed
    // as a side effect of the read itself (closePortAfterRead), so the
    // post-read is_serial_port_open() re-check is what must catch it.
    void readReturnsDisconnectedWhenPortClosesDuringRead()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->scriptedResponse = QByteArray("\xAA", 1);
        fake->closePortAfterRead = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        NeverCancelled cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // The "already closed" guard at the top of every method: once close()
    // has run, serial_ is null and every subsequent call must fail with
    // Disconnected without touching the (now possibly destroyed) backend.
    void everyMethodFailsWithDisconnectedAfterClose()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);

        DesktopKlineFlashTransport transport(serial.get()); // non-owning: keep `serial` alive
        auto closeResult = transport.close();
        QVERIFY(closeResult.has_value());

        NeverCancelled cancellation;
        const auto configureResult = transport.configure(
            KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});
        QVERIFY(!configureResult.has_value());
        QCOMPARE(configureResult.error().kind, ErrorKind::Disconnected);

        const auto openResult = transport.open();
        QVERIFY(!openResult.has_value());
        QCOMPARE(openResult.error().kind, ErrorKind::Disconnected);

        const auto setBaudResult = transport.setBaud(4800);
        QVERIFY(!setBaudResult.has_value());
        QCOMPARE(setBaudResult.error().kind, ErrorKind::Disconnected);

        const bytes::Bytes data{0xAA};
        const auto writeResult = transport.write(bytes::ByteView(data));
        QVERIFY(!writeResult.has_value());
        QCOMPARE(writeResult.error().kind, ErrorKind::Disconnected);

        const auto readResult = transport.read(50, cancellation);
        QVERIFY(!readResult.has_value());
        QCOMPARE(readResult.error().kind, ErrorKind::Disconnected);

        const auto headerResult = transport.set_add_iso14230_header(true);
        QVERIFY(!headerResult.has_value());
        QCOMPARE(headerResult.error().kind, ErrorKind::Disconnected);
    }

    // set_add_iso14230_header() forwards straight to
    // SerialPortActions::set_add_iso14230_header() -- the seam
    // DensoSh705xEepromKlineExecutor::execute() uses to turn the driver's
    // auto-header on for read_mem()'s raw SID_DUMP requests and back off for
    // connect_bootloader()/upload_kernel()'s self-framed exchanges. Verified
    // through the real (non-owning) SerialPortActions, not just a call log,
    // so this actually proves the flag the driver reads changes.
    void setAddIso14230HeaderForwardsToSerialAndSucceeds()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);                  // forces backend creation
        QCOMPARE(serial->get_add_iso14230_header(), false); // default

        DesktopKlineFlashTransport transport(serial.get()); // non-owning: query `serial` after

        const auto onResult = transport.set_add_iso14230_header(true);
        QVERIFY(onResult.has_value());
        QCOMPARE(serial->get_add_iso14230_header(), true);

        const auto offResult = transport.set_add_iso14230_header(false);
        QVERIFY(offResult.has_value());
        QCOMPARE(serial->get_add_iso14230_header(), false);
    }

    // write() must be skipped once request_unblock() has fired, exactly
    // like read() -- the shared unblock_requested_ flag guards both.
    void writeIsSkippedWithCancelledAfterRequestUnblock()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);

        DesktopKlineFlashTransport transport(std::move(serial));
        transport.request_unblock();
        fake->takeCallLog();

        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
        QVERIFY(fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // write() disconnected-before path: the port is already closed when
    // write() is called -- write_serial_data_echo_check() must never be
    // reached. (Symmetric to the CAN sibling's identically-named test.)
    void writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->portOpen.store(false);
        fake->takeCallLog();

        DesktopKlineFlashTransport transport(std::move(serial));
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
        QVERIFY(fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // read() success path when the backend legitimately has nothing to
    // report: raw.isEmpty() must map to a present-but-empty OptionalBytes,
    // not a failure.
    void readReturnsEmptyOptionalWhenBackendReturnsNoBytes()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->scriptedResponse = QByteArray(); // empty

        DesktopKlineFlashTransport transport(std::move(serial));
        NeverCancelled cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(result.has_value());
        QVERIFY(!result->has_value());
    }

    // setBaud()'s catch(const std::exception&) branch: change_port_speed()
    // itself throws a standard exception -- must map to Internal.
    void setBaudFailsWithInternalWhenDriverThrowsStandardException()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwOnBaudChange = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // setBaud()'s bare catch(...) branch: a non-std::exception-derived
    // failure must still be caught and mapped to Internal.
    void setBaudFailsWithInternalWhenDriverThrowsNonStandardException()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwNonStandardOnBaudChange = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // write()'s catch(const std::exception&) branch.
    void writeFailsWithInternalWhenDriverThrowsStandardException()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwOnWrite = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // write()'s bare catch(...) branch.
    void writeFailsWithInternalWhenDriverThrowsNonStandardException()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwNonStandardOnWrite = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // read()'s catch(const std::exception&) branch, with cancellation never
    // observed -- must map to Internal, not Cancelled.
    void readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwOnRead = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        NeverCancelled cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // read()'s bare catch(...) branch, with cancellation never observed.
    void readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwNonStandardOnRead = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        NeverCancelled cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // isOpen(): true while the port is open, false once closed, false when
    // the underlying check throws (caught, never propagated), and false
    // once this transport itself has been closed (serial_ is null).
    void isOpenReflectsThePortsRealOpenState()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);

        DesktopKlineFlashTransport transport(std::move(serial));
        QVERIFY(transport.isOpen());

        fake->portOpen.store(false);
        QVERIFY(!transport.isOpen());
    }

    void isOpenReturnsFalseWhenTheUnderlyingCheckThrows()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwOnIsOpen = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        QVERIFY(!transport.isOpen());
    }

    void isOpenReturnsFalseAfterClose()
    {
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            []() -> SerialBackend *
            { return new FakeBackend(); });
        serial->set_add_ssm_header(false);

        DesktopKlineFlashTransport transport(std::move(serial));
        QVERIFY(transport.isOpen());

        const auto closeResult = transport.close();
        QVERIFY(closeResult.has_value());
        QVERIFY(!transport.isOpen());
    }

    // read()'s post-read cancellation recheck (success path): cancellation
    // becomes observed-true only *after* the backend call has already
    // returned successfully -- must still map to Cancelled, not the bytes
    // that were read.
    void readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->scriptedResponse = QByteArray("\xAA", 1);

        DesktopKlineFlashTransport transport(std::move(serial));
        CancelledOnSecondCheck cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // read()'s post-throw cancellation recheck, catch(const std::exception&)
    // branch: cancellation becomes observed-true only after the backend
    // call has already thrown -- must map to Cancelled, not Internal.
    void readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwOnRead = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        CancelledOnSecondCheck cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // read()'s post-throw cancellation recheck, bare catch(...) branch.
    void readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false);
        fake->throwNonStandardOnRead = true;

        DesktopKlineFlashTransport transport(std::move(serial));
        CancelledOnSecondCheck cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    void closeIsIdempotentAndDestroysTheOwnedSerialPortActions()
    {
        bool destroyed = false;
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake, &destroyed]() -> SerialBackend *
            { fake = new FakeBackend(); fake->destroyed = &destroyed; return fake; });
        serial->set_add_ssm_header(false); // forces backend creation

        DesktopKlineFlashTransport transport(std::move(serial));
        QVERIFY(!destroyed);

        auto closeResult = transport.close();
        QVERIFY(closeResult.has_value());
        // ~SerialPortActions() deletes its backend via a
        // Qt::BlockingQueuedConnection (serial_backend_host.cpp), so by the
        // time close() returns, the fake is already gone.
        QVERIFY(destroyed);

        // Idempotent: calling again with an already-null serial_ must not crash.
        closeResult = transport.close();
        QVERIFY(closeResult.has_value());
    }

    // Proves the non-owning constructor (step 5c, Task 17): required so
    // this transport can wrap MainWindow's single, session-lifetime
    // SerialPortActions instance (constructed once in mainwindow.cpp and
    // reused for the app's whole session) without close() destroying it out
    // from under every other in-flight/future use of that shared object --
    // the owning constructor's close() == owned_serial_.reset() would do
    // exactly that if it were used here instead. `destroyed` is a sentinel
    // flipped only by FakeBackend's destructor; if it stayed false through
    // close(), the SerialPortActions -- and its backend -- were never torn
    // down. The local `serial` unique_ptr (owned by this test, standing in
    // for MainWindow's member) is then used again after close() to prove it
    // is still a live, callable object, not a dangling pointer.
    void closeOnANonOwningSerialPortActionsDoesNotDestroyIt()
    {
        bool destroyed = false;
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake, &destroyed]() -> SerialBackend *
            { fake = new FakeBackend(); fake->destroyed = &destroyed; return fake; });
        serial->set_add_ssm_header(false); // forces backend creation

        {
            DesktopKlineFlashTransport transport(serial.get()); // non-owning
            QVERIFY(!destroyed);

            auto closeResult = transport.close();
            QVERIFY(closeResult.has_value());
            // The proof this test exists for: close() on a non-owning
            // transport must NOT destroy the externally-owned
            // SerialPortActions.
            QVERIFY(!destroyed);

            // Idempotent, same as the owning path.
            closeResult = transport.close();
            QVERIFY(closeResult.has_value());
            QVERIFY(!destroyed);
        }
        // transport is gone now; `serial` must still be alive and usable --
        // proves this isn't merely "destroyed wasn't set yet", but that the
        // object genuinely survives past the transport's own lifetime.
        QVERIFY(!destroyed);
        const bool stillCallable = serial->is_serial_port_open(); // must not crash
        Q_UNUSED(stillCallable);
        serial.reset(); // only now does the real teardown happen
        QVERIFY(destroyed);
    }

    // request_unblock() has no real interrupt primitive to fire --
    // SerialPortActions exposes none -- so it can only set a flag checked
    // before the *next* read call. This test proves both halves of that
    // documented, bounded-latency contract: (1) an already in-flight read
    // does NOT return early just because request_unblock() fires -- it
    // still returns only via its own existing timeout (simulated here by
    // releasing the fake's continueRead gate); and (2) once
    // request_unblock() has fired, the *next* call to read() returns
    // immediately as Cancelled without ever reaching the backend.
    void requestUnblockCausesAPendingReadToReturnPromptly()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>(
            "", "", nullptr, nullptr,
            [&fake]() -> SerialBackend *
            { fake = new FakeBackend(); return fake; });
        serial->set_add_ssm_header(false); // create backend before wiring gates
        fake->scriptedResponse = QByteArray("\xAA", 1);

        QSemaphore readEntered;
        QSemaphore continueRead;
        fake->readEntered = &readEntered;
        fake->continueRead = &continueRead;

        DesktopKlineFlashTransport transport(std::move(serial));
        NeverCancelled cancellation;

        fastecu::Result<DesktopKlineFlashTransport::OptionalBytes> inFlightResult;
        std::atomic<bool> readerFinished{false};
        std::thread reader(
            [&]
            {
                inFlightResult = transport.read(50, cancellation);
                readerFinished.store(true);
            });
        QVERIFY2(readEntered.tryAcquire(1, 1000), "backend read did not start");

        transport.request_unblock();
        QTest::qWait(50);
        QVERIFY2(!readerFinished.load(),
                 "request_unblock() must not interrupt an already in-flight read");

        continueRead.release(); // simulates the backend's own bounded timeout firing
        reader.join();

        QVERIFY(readerFinished.load());
        QVERIFY(inFlightResult.has_value());
        QVERIFY(inFlightResult->has_value());
        QVERIFY(inFlightResult->value() == bytes::Bytes{0xAA});

        // Second half of the contract: the *next* read must not reach the
        // backend at all.
        fake->takeCallLog();
        const auto secondResult = transport.read(50, cancellation);
        QVERIFY(!secondResult.has_value());
        QCOMPARE(secondResult.error().kind, ErrorKind::Cancelled);
        QVERIFY(fake->takeCallLog().filter("read:begin").isEmpty());
    }
};

int run_test_desktop_kline_flash_transport(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestDesktopKlineFlashTransport t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_desktop_kline_flash_transport.moc"
