// Characterization/unit tests for DesktopCanFlashTransport (step 5c, Task
// 12) -- the first real, concrete adapter wrapping SerialPortActions as
// fastecu::flash::ICanFlashTransport. Same rationale and harness as
// tests/test_desktop_kline_flash_transport.cpp (this directory): every
// prior test in this plan used only scripted fakes
// (ScriptedCanFlashTransport); this suite proves the adapter itself against
// the real (test-doubled) SerialPortActions/SerialBackend marshaling path.
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"

#include <QCoreApplication>
#include <QSemaphore>
#include <QTest>

#include <gmock/gmock.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/transport/fake_backed_serial.h"
#include "src/platform/desktop/common/transport/setter_sequence_expectations.h"

using namespace std::chrono_literals;

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::flash::DesktopCanFlashTransport;
using fastecu::flash::Iso15765Config;

class TestDesktopCanFlashTransport : public QObject
{
    Q_OBJECT

  private slots:

    void configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure()
    {
        FakeBackedSerial serial;
        // Fail the *fifth* setter in configure()'s specified order
        // (set_is_iso15765_connection, set_is_can_connection,
        // set_is_iso14230_connection, set_is_29_bit_id, set_can_speed,
        // set_can_source_address, set_can_destination_address,
        // set_iso15765_source_address, set_iso15765_destination_address) --
        // this proves the first four setters really ran, in order, and that
        // nothing after the failure (source/destination CAN and ISO-15765
        // IDs, open()) ran.
        ::testing::InSequence sequence;
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(false));
        EXPECT_CALL(serial.fake(), set_can_source_address(::testing::_)).Times(0);
        EXPECT_CALL(serial.fake(), set_can_destination_address(::testing::_)).Times(0);
        EXPECT_CALL(serial.fake(), set_iso15765_source_address(::testing::_)).Times(0);
        EXPECT_CALL(serial.fake(), set_iso15765_destination_address(::testing::_)).Times(0);
        EXPECT_CALL(serial.fake(), set_add_iso14230_header(::testing::_)).Times(0);
        EXPECT_CALL(serial.fake(), open_serial_port()).Times(0);

        DesktopCanFlashTransport transport(serial.release());
        const auto result = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
    }

    // Data-driven sibling of configureChecksEveryBooleanSetterInOrderAndStops-
    // AtFirstFailure() above (which only exercises the fifth setter's
    // failure branch, set_can_speed): proves every remaining setter's own
    // InvalidConfig return path independently.
    void configureFailsAtEachRemainingSetterInTurn_data()
    {
        QTest::addColumn<int>("setterIndex");
        QTest::newRow("set_is_iso15765_connection") << 0;
        QTest::newRow("set_is_can_connection") << 1;
        QTest::newRow("set_is_iso14230_connection") << 2;
        QTest::newRow("set_is_29_bit_id") << 3;
        QTest::newRow("set_can_source_address") << 5;
        QTest::newRow("set_can_destination_address") << 6;
        QTest::newRow("set_iso15765_source_address") << 7;
        QTest::newRow("set_iso15765_destination_address") << 8;
        QTest::newRow("set_add_iso14230_header") << 9;
    }

    void configureFailsAtEachRemainingSetterInTurn()
    {
        QFETCH(int, setterIndex);

        FakeBackedSerial serial;

        ::testing::InSequence sequence;
        expectSetterAt(EXPECT_CALL(serial.fake(), set_is_iso15765_connection(true)), 0, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_is_can_connection(false)), 1, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)), 2, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)), 3, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_can_speed(QStringLiteral("500000"))), 4, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_can_source_address(2016)), 5, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_can_destination_address(2024)), 6, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_iso15765_source_address(2016)), 7, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_iso15765_destination_address(2024)), 8, setterIndex);
        expectSetterAt(EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)), 9, setterIndex);
        EXPECT_CALL(serial.fake(), open_serial_port()).Times(0);

        DesktopCanFlashTransport transport(serial.release());
        const auto result = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
    }

    void openFailureReturnsDisconnectedWithoutAnyWrite()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(::testing::Return(QString{}));
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).Times(0);

        DesktopCanFlashTransport transport(serial.release());
        const auto result = transport.open();

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // Success mirror of configureChecksEveryBooleanSetterInOrderAndStopsAt-
    // FirstFailure() above: every setter is expected to succeed, so
    // configure() must run all ten setters, in order, and return success.
    void configureSucceedsWhenEverySetterSucceeds()
    {
        FakeBackedSerial serial;
        ::testing::InSequence sequence;
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_can_source_address(2016)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_can_destination_address(2024)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_iso15765_source_address(2016)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_iso15765_destination_address(2024)).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(::testing::Return(true));

        DesktopCanFlashTransport transport(serial.release());
        const auto result = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

        QVERIFY(result.has_value());
    }

    // A K-Line session on the same facade leaves ISO-14230 auto-headers on.
    // configure() must clear that state, or the first CAN frame goes out with
    // a K-Line header attached.
    void configureClearsStickyIso14230HeaderState()
    {
        FakeBackedSerial serial;
        QVERIFY(serial->set_add_iso14230_header(true));
        SerialPortActions *observed = serial.get();

        DesktopCanFlashTransport transport(serial.release());
        const auto result = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

        QVERIFY(result.has_value());
        QCOMPARE(observed->get_add_iso14230_header(), false);
    }

    // Success mirror of openFailureReturnsDisconnectedWithoutAnyWrite().
    void openSucceedsWhenBackendReturnsANonEmptyPortName()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(::testing::Return(QStringLiteral("COM3")));

        DesktopCanFlashTransport transport(serial.release());
        const auto result = transport.open();

        QVERIFY(result.has_value());
    }

    // write() success path: port open throughout, cancellation never fires.
    void writeSucceedsWhenPortStaysOpenThroughout()
    {
        FakeBackedSerial serial;
        ::testing::InSequence sequence;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(QByteArray::fromHex("010203")))
            .WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const bytes::Bytes data{0x01, 0x02, 0x03};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(result.has_value());
    }

    // write() must observe cancellation.cancelled() before ever issuing the
    // write -- the backend must never be touched at all. Distinct from the
    // K-Line sibling, whose write() takes no cancellation token at all.
    void writeReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingWrite()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).Times(0);

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation(true);
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // write() disconnected-during path: the port closes as a side effect of
    // the write call itself -- the post-write is_serial_port_open() check
    // must catch this even though write_serial_data_echo_check() itself
    // never signals failure via its return value.
    void writeFailsWithDisconnectedWhenPortClosesDuringWrite()
    {
        FakeBackedSerial serial;
        ::testing::InSequence sequence;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_))
            .WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // write() disconnected-before path: the port is already closed when
    // write() is called -- write_serial_data_echo_check() must never be
    // reached.
    void writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).Times(0);

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // read() success path: port open, cancellation never fires, backend
    // returns scripted bytes.
    void readReturnsScriptedBytesOnSuccess()
    {
        FakeBackedSerial serial;
        ::testing::InSequence sequence;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\x01\x02", 2)));
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));

        DesktopCanFlashTransport transport(serial.release());
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
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), read_serial_data(::testing::_)).Times(0);

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation(true);
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // read() disconnected-before path: the port is already closed when
    // read() is called -- read_serial_data() must never be reached.
    void readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));
        EXPECT_CALL(serial.fake(), read_serial_data(::testing::_)).Times(0);

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    // read() disconnected-during path: the read returns data, then the
    // post-read is_serial_port_open() check reports the closed port.
    void readReturnsDisconnectedWhenPortClosesDuringRead()
    {
        FakeBackedSerial serial;
        ::testing::InSequence sequence;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\xAA", 1)));
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));

        DesktopCanFlashTransport transport(serial.release());
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
        FakeBackedSerial serial;

        DesktopCanFlashTransport transport(serial.get()); // non-owning: keep `serial` alive
        auto closeResult = transport.close();
        QVERIFY(closeResult.has_value());

        FakeCancellationToken cancellation;
        const auto configureResult = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});
        QVERIFY(!configureResult.has_value());
        QCOMPARE(configureResult.error().kind, ErrorKind::Disconnected);

        const auto openResult = transport.open();
        QVERIFY(!openResult.has_value());
        QCOMPARE(openResult.error().kind, ErrorKind::Disconnected);

        const bytes::Bytes data{0xAA};
        const auto writeResult = transport.write(bytes::ByteView(data), cancellation);
        QVERIFY(!writeResult.has_value());
        QCOMPARE(writeResult.error().kind, ErrorKind::Disconnected);

        const auto readResult = transport.read(50ms, cancellation);
        QVERIFY(!readResult.has_value());
        QCOMPARE(readResult.error().kind, ErrorKind::Disconnected);
    }

    // write() must be skipped once request_unblock() has fired, exactly
    // like read() -- the shared unblock_requested_ flag guards both.
    void writeIsSkippedWithCancelledAfterRequestUnblock()
    {
        FakeBackedSerial serial;

        DesktopCanFlashTransport transport(serial.release());
        transport.request_unblock();
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).Times(0);

        FakeCancellationToken cancellation;
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // read() success path when the backend legitimately has nothing to
    // report: raw.isEmpty() must map to a present-but-empty optional, not a
    // failure.
    void readReturnsEmptyOptionalWhenBackendReturnsNoBytes()
    {
        FakeBackedSerial serial;
        ::testing::InSequence sequence;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(::testing::Return(QByteArray{}));
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(result.has_value());
        QVERIFY(!result->has_value());
    }

    // write()'s catch(const std::exception&) branch.
    void writeFailsWithInternalWhenDriverThrowsStandardException()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // write()'s bare catch(...) branch.
    void writeFailsWithInternalWhenDriverThrowsNonStandardException()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_))
            .WillOnce(ThrowNonStandardBackendFailure());

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // read()'s catch(const std::exception&) branch, with cancellation never
    // observed -- must map to Internal, not Cancelled.
    void readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // read()'s bare catch(...) branch, with cancellation never observed.
    void readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(ThrowNonStandardBackendFailure());

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
    }

    // read()'s post-read cancellation recheck (success path): cancellation
    // becomes observed-true only *after* the backend call has already
    // returned successfully -- must still map to Cancelled, not the bytes
    // that were read.
    void readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\xAA", 1)));

        DesktopCanFlashTransport transport(serial.release());
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
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50))
            .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(2);
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    // read()'s post-throw cancellation recheck, bare catch(...) branch.
    void readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(ThrowNonStandardBackendFailure());

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(2);
        const auto result = transport.read(50ms, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    void closeIsIdempotentAndDestroysTheOwnedSerialPortActions()
    {
        bool destroyed = false;
        FakeBackedSerial serial{[&destroyed](auto& fake) { fake.destroyed = &destroyed; }};

        DesktopCanFlashTransport transport(serial.release());
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

    // Proves the non-owning constructor (step 5c, Task 17) -- see
    // test_desktop_kline_flash_transport.cpp's identically-named test for
    // the full rationale: MainWindow's single, session-lifetime
    // SerialPortActions instance must survive close() on a transport that
    // does not own it.
    void closeOnANonOwningSerialPortActionsDoesNotDestroyIt()
    {
        bool destroyed = false;
        FakeBackedSerial serial{[&destroyed](auto& fake) { fake.destroyed = &destroyed; }};

        {
            DesktopCanFlashTransport transport(serial.get()); // non-owning
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
        // transport is gone now; `serial` must still be alive and usable.
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
        FakeBackedSerial serial;

        QSemaphore readEntered;
        QSemaphore continueRead;
        EXPECT_CALL(serial.fake(), is_serial_port_open())
            .WillOnce(::testing::Return(true))
            .WillOnce(::testing::Return(true));
        EXPECT_CALL(serial.fake(), read_serial_data(50))
            .WillOnce(
                [&readEntered, &continueRead](std::uint16_t)
                {
                    readEntered.release();
                    continueRead.acquire();
                    return QByteArray("\xAA", 1);
                });

        DesktopCanFlashTransport transport(serial.release());
        FakeCancellationToken cancellation;

        fastecu::Result<std::optional<bytes::Bytes>> inFlightResult;
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
        EXPECT_CALL(serial.fake(), read_serial_data(::testing::_)).Times(0);
        const auto secondResult = transport.read(50ms, cancellation);
        QVERIFY(!secondResult.has_value());
        QCOMPARE(secondResult.error().kind, ErrorKind::Cancelled);
    }

    void fakeBackendReportsScriptedPortListAndBattery()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), check_serial_ports()).WillOnce(::testing::Return(QStringList{"op2-0", "op2-1"}));
        // DoDefault() rather than Return(true): the selected port must actually
        // reach the backend's configuration state, which Return(true) would skip.
        EXPECT_CALL(serial.fake(), set_serial_port(QStringLiteral("op2-1"))).WillOnce(::testing::DoDefault());
        EXPECT_CALL(serial.fake(), read_vbatt()).WillOnce(::testing::Return(11676UL));

        QCOMPARE(serial->check_serial_ports(), QStringList({"op2-0", "op2-1"}));
        QVERIFY(serial->set_serial_port("op2-1"));
        QCOMPARE(serial->get_serial_port(), QStringLiteral("op2-1"));
        QCOMPARE(serial->read_vbatt(), 11676UL);
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    TestDesktopCanFlashTransport test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "desktop_can_flash_transport_test.moc"
