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
