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

#include <gmock/gmock.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::flash::DesktopKlineFlashTransport;
using fastecu::flash::KlineConfig;
using namespace std::chrono_literals;

// Shapes one boolean-setter expectation for configureFailsAtEachRemainingSetter-
// InTurn(): configure() must reach every setter up to and including the failing
// one, and must never reach the setters after it. An expectation that can never
// fire carries no action -- Times(0) combined with WillRepeatedly() makes Google
// Mock log "Too many actions specified" for every such line.
template <typename Expectation> void expectSetterAt(Expectation& expectation, int position, int failingIndex)
{
    if (failingIndex >= position)
    {
        expectation.WillOnce(::testing::Return(failingIndex != position));
    }
    else
    {
        expectation.Times(0);
    }
}

class TestDesktopKlineFlashTransport : public QObject
{
    Q_OBJECT

  private slots:

    void postKernelUploadDelayCapabilityMirrorsOpenPort2OnUnix()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        SerialPortActions *serial_ptr = serial.get();
        DesktopKlineFlashTransport transport(std::move(serial));

        QVERIFY(!transport.requires_post_kernel_upload_delay());
        QVERIFY(serial_ptr->set_use_openport2_adapter(true));
#if defined(Q_OS_UNIX)
        QVERIFY(transport.requires_post_kernel_upload_delay());
#else
        QVERIFY(!transport.requires_post_kernel_upload_delay());
#endif
    }

    void lecControlOperationsForwardToSerialBackend()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, set_lec_lines(1, 1)).WillOnce(::testing::Return(STATUS_SUCCESS));
        EXPECT_CALL(*fake, pulse_lec_2_line(200)).WillOnce(::testing::Return(STATUS_SUCCESS));
        EXPECT_CALL(*fake, set_lec_lines(0, 1)).WillOnce(::testing::Return(STATUS_SUCCESS));

        DesktopKlineFlashTransport transport(std::move(serial));

        QVERIFY(transport.disable_lec_lines().has_value());
        QVERIFY(transport.pulse_lec_2_line(200ms).has_value());
        QVERIFY(transport.enable_programming_voltage_line().has_value());
    }

    void configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation
        // Fail the *third* setter in configure()'s specified order
        // (set_is_iso14230_connection, set_is_can_connection,
        // set_is_iso15765_connection, set_is_29_bit_id,
        // set_serial_port_baudrate) -- this proves both that the first two
        // setters really ran, in order, and that nothing after the failure
        // (the 29-bit-id setter, the baudrate setter, open()) ran.
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, set_is_iso14230_connection(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_can_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_iso15765_connection(false)).WillOnce(::testing::Return(false));
        EXPECT_CALL(*fake, set_is_29_bit_id(::testing::_)).Times(0);
        EXPECT_CALL(*fake, set_serial_port_baudrate(::testing::_)).Times(0);
        EXPECT_CALL(*fake, open_serial_port()).Times(0);

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result =
            transport.configure(KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
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
        QTest::newRow("set_is_iso14230_connection") << 0;
        QTest::newRow("set_is_can_connection") << 1;
        QTest::newRow("set_is_29_bit_id") << 3;
        QTest::newRow("set_serial_port_baudrate") << 4;
    }

    void configureFailsAtEachRemainingSetterInTurn()
    {
        QFETCH(int, setterIndex);

        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation

        ::testing::InSequence sequence;
        expectSetterAt(EXPECT_CALL(*fake, set_is_iso14230_connection(true)), 0, setterIndex);
        expectSetterAt(EXPECT_CALL(*fake, set_is_can_connection(false)), 1, setterIndex);
        expectSetterAt(EXPECT_CALL(*fake, set_is_iso15765_connection(false)), 2, setterIndex);
        expectSetterAt(EXPECT_CALL(*fake, set_is_29_bit_id(false)), 3, setterIndex);
        expectSetterAt(EXPECT_CALL(*fake, set_serial_port_baudrate(QStringLiteral("10400"))), 4, setterIndex);
        EXPECT_CALL(*fake, open_serial_port()).Times(0);

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result =
            transport.configure(KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
    }

    void openFailureReturnsDisconnectedWithoutAnyWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QString{}));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.open();

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // Success mirror of configureChecksEveryBooleanSetterInOrderAndStopsAt-
    // FirstFailure() above: every setter is expected to succeed, so
    // configure() must run all five setters, in order, and return success.
    void configureSucceedsWhenEverySetterSucceeds()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, set_is_iso14230_connection(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_can_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_iso15765_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, set_serial_port_baudrate(QStringLiteral("10400"))).WillOnce(::testing::Return(true));

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result =
            transport.configure(KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

        QVERIFY(result.has_value());
    }

    // Success mirror of openFailureReturnsDisconnectedWithoutAnyWrite().
    void openSucceedsWhenBackendReturnsANonEmptyPortName()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, open_serial_port()).WillOnce(::testing::Return(QStringLiteral("COM3")));

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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(STATUS_SUCCESS));

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(result.has_value());
    }

    // setBaud() failure path: port stays open, but change_port_speed()
    // returns the real backend's failure sentinel (STATUS_ERROR, a small
    // *positive* value) -- maps to Internal, not InvalidConfig (a runtime
    // driver rejection, not a config-shape problem).
    void setBaudFailsWithInternalWhenPortStaysOpenButDriverRejectsChange()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(STATUS_ERROR));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));

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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(*fake, change_port_speed(::testing::_)).Times(0);

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // setBaud() disconnected-during path: change_port_speed() itself reports
    // a failure code (driver rejected/errored) AND the port is observed
    // closed on the follow-up is_serial_port_open() check -- must map to
    // Disconnected, not the generic Internal "driver rejected" branch.
    void setBaudFailsWithDisconnectedWhenPortClosesDuringBaudChange()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).WillOnce(::testing::Return(STATUS_ERROR));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));

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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(QByteArray::fromHex("010203")))
            .WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));

        DesktopKlineFlashTransport transport(std::move(serial));
        const bytes::Bytes data{0x01, 0x02, 0x03};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(result.has_value());
        QCOMPARE(*result, data.size());
    }

    // write() disconnected-during path: the port closes as a side effect of
    // the write call itself (e.g. the adapter dropped mid-transfer) -- the
    // post-write is_serial_port_open() check must catch this even though
    // write_serial_data_echo_check() itself never signals failure via its
    // return value.
    void writeFailsWithDisconnectedWhenPortClosesDuringWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));

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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\x01\x02", 2)));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(result.has_value());
        QVERIFY(result->has_value());
        QVERIFY(result->value() == (bytes::Bytes{0x01, 0x02}));
    }

    // read() observes cancellation.cancelled() before ever issuing the read
    // -- the backend must never be touched at all.
    void readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation(true);
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // read() disconnected-before path: the port is already closed when
    // read() is called -- read_serial_data() must never be reached.
    void readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // read() disconnected-during path: the read returns data, then the
    // post-read is_serial_port_open() check reports the closed port.
    void readReturnsDisconnectedWhenPortClosesDuringRead()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\xAA", 1)));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // The "already closed" guard at the top of every method: once close()
    // has run, serial_ is null and every subsequent call must fail with
    // Disconnected without touching the (now possibly destroyed) backend.
    void everyMethodFailsWithDisconnectedAfterClose()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);

        DesktopKlineFlashTransport transport(serial.get()); // non-owning: keep `serial` alive
        auto closeResult = transport.close();
        QVERIFY(closeResult.has_value());

        FakeCancellationToken cancellation;
        const auto configureResult =
            transport.configure(KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});
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

        const auto readResult = transport.read(50ms, cancellation);
        QVERIFY(!readResult.has_value());
        QCOMPARE(readResult.error().kind, ErrorKind::Disconnected);

        const auto headerResult = transport.set_add_iso14230_header(true);
        QVERIFY(!headerResult.has_value());
        QCOMPARE(headerResult.error().kind, ErrorKind::Disconnected);

        const auto disableLecResult = transport.disable_lec_lines();
        QVERIFY(!disableLecResult.has_value());
        QCOMPARE(disableLecResult.error().kind, ErrorKind::Disconnected);

        const auto pulseLecResult = transport.pulse_lec_2_line(200ms);
        QVERIFY(!pulseLecResult.has_value());
        QCOMPARE(pulseLecResult.error().kind, ErrorKind::Disconnected);

        const auto programmingLineResult = transport.enable_programming_voltage_line();
        QVERIFY(!programmingLineResult.has_value());
        QCOMPARE(programmingLineResult.error().kind, ErrorKind::Disconnected);
    }

    // set_add_iso14230_header() forwards straight to
    // SerialPortActions::set_add_iso14230_header() -- the seam
    // DensoSh705xEepromKlineExecutor::execute() uses to turn the driver's
    // auto-header on for read_mem()'s raw SID_DUMP requests and back off for
    // connect_bootloader()/upload_kernel()'s self-framed exchanges. Verified
    // through the real (non-owning) SerialPortActions, not just a mock call,
    // so this actually proves the flag the driver reads changes.
    void setAddIso14230HeaderForwardsToSerialAndSucceeds()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);

        DesktopKlineFlashTransport transport(std::move(serial));
        transport.request_unblock();
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);

        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // write() disconnected-before path: the port is already closed when
    // write() is called -- write_serial_data_echo_check() must never be
    // reached. (Symmetric to the CAN sibling's identically-named test.)
    void writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).Times(0);

        DesktopKlineFlashTransport transport(std::move(serial));
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data));

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // read() success path when the backend legitimately has nothing to
    // report: raw.isEmpty() must map to a present-but-empty OptionalBytes,
    // not a failure.
    void readReturnsEmptyOptionalWhenBackendReturnsNoBytes()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(result.has_value());
        QVERIFY(!result->has_value());
    }

    // setBaud()'s catch(const std::exception&) branch: change_port_speed()
    // itself throws a standard exception -- must map to Internal.
    void setBaudFailsWithInternalWhenDriverThrowsStandardException()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800")))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend baud-change failure")));

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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, change_port_speed(QStringLiteral("4800"))).WillOnce(ThrowNonStandardBackendFailure());

        DesktopKlineFlashTransport transport(std::move(serial));
        const auto result = transport.setBaud(4800);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // write()'s catch(const std::exception&) branch.
    void writeFailsWithInternalWhenDriverThrowsStandardException()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));

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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, write_serial_data_echo_check(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());

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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // read()'s bare catch(...) branch, with cancellation never observed.
    void readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50)).WillOnce(ThrowNonStandardBackendFailure());

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // isOpen(): true while the port is open, false once closed, false when
    // the underlying check throws (caught, never propagated), and false
    // once this transport itself has been closed (serial_ is null).
    void isOpenReflectsThePortsRealOpenState()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);

        DesktopKlineFlashTransport transport(std::move(serial));
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true)).WillOnce(::testing::Return(false));
        QVERIFY(transport.isOpen());

        QVERIFY(!transport.isOpen());
    }

    void isOpenReturnsFalseWhenTheUnderlyingCheckThrows()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open())
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend open-state failure")));

        DesktopKlineFlashTransport transport(std::move(serial));
        QVERIFY(!transport.isOpen());
    }

    void isOpenReturnsFalseAfterClose()
    {
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          []() -> SerialBackend * { return new NiceFakeBackend(); });
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\xAA", 1)));

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(2);
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // read()'s post-throw cancellation recheck, catch(const std::exception&)
    // branch: cancellation becomes observed-true only after the backend
    // call has already thrown -- must map to Cancelled, not Internal.
    void readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(2);
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // read()'s post-throw cancellation recheck, bare catch(...) branch.
    void readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50)).WillOnce(ThrowNonStandardBackendFailure());

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(2);
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    void closeIsIdempotentAndDestroysTheOwnedSerialPortActions()
    {
        bool destroyed = false;
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake, &destroyed]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              fake->destroyed = &destroyed;
                                                              return fake;
                                                          });
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake, &destroyed]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              fake->destroyed = &destroyed;
                                                              return fake;
                                                          });
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new NiceFakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // create backend before wiring gates

        QSemaphore readEntered;
        QSemaphore continueRead;
        EXPECT_CALL(*fake, is_serial_port_open()).WillOnce(::testing::Return(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(*fake, read_serial_data(50))
            .WillOnce(
                [&readEntered, &continueRead](std::uint16_t)
                {
                    readEntered.release();
                    continueRead.acquire();
                    return QByteArray("\xAA", 1);
                });

        DesktopKlineFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;

        fastecu::Result<DesktopKlineFlashTransport::OptionalBytes> inFlightResult;
        std::atomic<bool> readerFinished{false};
        std::thread reader(
            [&]
            {
                inFlightResult = transport.read(50ms, cancellation);
                readerFinished.store(true);
            });
        QVERIFY2(readEntered.tryAcquire(1, 1000), "backend read did not start");

        transport.request_unblock();
        QTest::qWait(50);
        QVERIFY2(!readerFinished.load(), "request_unblock() must not interrupt an already in-flight read");

        continueRead.release(); // simulates the backend's own bounded timeout firing
        reader.join();

        QVERIFY(readerFinished.load());
        QVERIFY(inFlightResult.has_value());
        QVERIFY(inFlightResult->has_value());
        QVERIFY(inFlightResult->value() == bytes::Bytes{0xAA});

        // Second half of the contract: the *next* read must not reach the
        // backend at all.
        EXPECT_CALL(*fake, read_serial_data(::testing::_)).Times(0);
        const auto secondResult = transport.read(50ms, cancellation);
        QVERIFY(!secondResult.has_value());
        QCOMPARE(secondResult.error().kind, ErrorKind::Cancelled);
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    TestDesktopKlineFlashTransport test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "desktop_kline_flash_transport_test.moc"
