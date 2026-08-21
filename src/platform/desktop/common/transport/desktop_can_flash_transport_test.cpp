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

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

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
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation
        fake->takeCallLog();
        // Fail the *fifth* setter in configure()'s specified order
        // (set_is_iso15765_connection, set_is_can_connection,
        // set_is_iso14230_connection, set_is_29_bit_id, set_can_speed,
        // set_can_source_address, set_can_destination_address,
        // set_iso15765_source_address, set_iso15765_destination_address) --
        // this proves the first four setters really ran, in order, and that
        // nothing after the failure (source/destination CAN and ISO-15765
        // IDs, open()) ran.
        fake->canSpeedResult = false;

        DesktopCanFlashTransport transport(std::move(serial));
        const auto result = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
        QCOMPARE(fake->takeCallLog(), QStringList({"cfg:set_is_iso15765_connection:1", "cfg:set_is_can_connection:0",
                                                   "cfg:set_is_iso14230_connection:0", "cfg:set_is_29_bit_id:0",
                                                   "cfg:set_can_speed:500000"}));
    }

    // Data-driven sibling of configureChecksEveryBooleanSetterInOrderAndStops-
    // AtFirstFailure() above (which only exercises the fifth setter's
    // failure branch, set_can_speed): proves every remaining setter's own
    // InvalidConfig return path independently.
    void configureFailsAtEachRemainingSetterInTurn_data()
    {
        QTest::addColumn<int>("setterIndex");
        QTest::addColumn<int>("expectedLogCount");
        QTest::newRow("set_is_iso15765_connection") << 0 << 1;
        QTest::newRow("set_is_can_connection") << 1 << 2;
        QTest::newRow("set_is_iso14230_connection") << 2 << 3;
        QTest::newRow("set_is_29_bit_id") << 3 << 4;
        QTest::newRow("set_can_source_address") << 5 << 6;
        QTest::newRow("set_can_destination_address") << 6 << 7;
        QTest::newRow("set_iso15765_source_address") << 7 << 8;
        QTest::newRow("set_iso15765_destination_address") << 8 << 9;
    }

    void configureFailsAtEachRemainingSetterInTurn()
    {
        QFETCH(int, setterIndex);
        QFETCH(int, expectedLogCount);

        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation
        fake->takeCallLog();

        switch (setterIndex)
        {
        case 0:
            fake->isIso15765ConnectionResult = false;
            break;
        case 1:
            fake->isCanConnectionResult = false;
            break;
        case 2:
            fake->isIso14230ConnectionResult = false;
            break;
        case 3:
            fake->is29BitIdResult = false;
            break;
        case 5:
            fake->canSourceAddressResult = false;
            break;
        case 6:
            fake->canDestinationAddressResult = false;
            break;
        case 7:
            fake->iso15765SourceAddressResult = false;
            break;
        case 8:
            fake->iso15765DestinationAddressResult = false;
            break;
        }

        DesktopCanFlashTransport transport(std::move(serial));
        const auto result = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
        QCOMPARE(fake->takeCallLog().size(), expectedLogCount);
    }

    void openFailureReturnsDisconnectedWithoutAnyWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->takeCallLog();
        // FakeBackend::open_serial_port() defaults to the real backend's
        // "couldn't open the port" failure sentinel: an empty QString
        // (confirmed against SerialPortActionsDirect::open_serial_port(),
        // serial_port_actions_direct.cpp:519-644).

        DesktopCanFlashTransport transport(std::move(serial));
        const auto result = transport.open();

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
        QVERIFY(fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // Success mirror of configureChecksEveryBooleanSetterInOrderAndStopsAt-
    // FirstFailure() above: every FakeBackend setter control defaults to
    // true, so configure() must run all nine setters, in order, and return
    // success.
    void configureSucceedsWhenEverySetterSucceeds()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation
        fake->takeCallLog();

        DesktopCanFlashTransport transport(std::move(serial));
        const auto result = transport.configure(
            Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

        QVERIFY(result.has_value());
        QCOMPARE(fake->takeCallLog(),
                 QStringList({"cfg:set_is_iso15765_connection:1", "cfg:set_is_can_connection:0",
                              "cfg:set_is_iso14230_connection:0", "cfg:set_is_29_bit_id:0", "cfg:set_can_speed:500000",
                              "cfg:set_can_source_address:2016", "cfg:set_can_destination_address:2024",
                              "cfg:set_iso15765_source_address:2016", "cfg:set_iso15765_destination_address:2024"}));
    }

    // Success mirror of openFailureReturnsDisconnectedWithoutAnyWrite():
    // FakeBackend::open_serial_port() returns whatever openSerialPortResult
    // is set to; a non-empty string is the real backend's success sentinel.
    void openSucceedsWhenBackendReturnsANonEmptyPortName()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->openSerialPortResult = "COM3";

        DesktopCanFlashTransport transport(std::move(serial));
        const auto result = transport.open();

        QVERIFY(result.has_value());
    }

    // write() success path: port open throughout, cancellation never fires.
    void writeSucceedsWhenPortStaysOpenThroughout()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->takeCallLog();

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const bytes::Bytes data{0x01, 0x02, 0x03};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(result.has_value());
        QVERIFY(!fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // write() must observe cancellation.cancelled() before ever issuing the
    // write -- the backend must never be touched at all. Distinct from the
    // K-Line sibling, whose write() takes no cancellation token at all.
    void writeReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->takeCallLog();

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation(true);
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
        QVERIFY(fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // write() disconnected-during path: the port closes as a side effect of
    // the write call itself -- the post-write is_serial_port_open() check
    // must catch this even though write_serial_data_echo_check() itself
    // never signals failure via its return value.
    void writeFailsWithDisconnectedWhenPortClosesDuringWrite()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->closePortAfterWrite = true;

        DesktopCanFlashTransport transport(std::move(serial));
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
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->portOpen.store(false);
        fake->takeCallLog();

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
        QVERIFY(fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // read() success path: port open, cancellation never fires, backend
    // returns scripted bytes.
    void readReturnsScriptedBytesOnSuccess()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->scriptedResponse = QByteArray("\x01\x02", 2);

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->takeCallLog();

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation(true);
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->portOpen.store(false);
        fake->takeCallLog();

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->scriptedResponse = QByteArray("\xAA", 1);
        fake->closePortAfterRead = true;

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);

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

        const auto readResult = transport.read(50, cancellation);
        QVERIFY(!readResult.has_value());
        QCOMPARE(readResult.error().kind, ErrorKind::Disconnected);
    }

    // write() must be skipped once request_unblock() has fired, exactly
    // like read() -- the shared unblock_requested_ flag guards both.
    void writeIsSkippedWithCancelledAfterRequestUnblock()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);

        DesktopCanFlashTransport transport(std::move(serial));
        transport.request_unblock();
        fake->takeCallLog();

        FakeCancellationToken cancellation;
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
        QVERIFY(fake->takeCallLog().filter("write_echo_check:begin:").isEmpty());
    }

    // read() success path when the backend legitimately has nothing to
    // report: raw.isEmpty() must map to a present-but-empty optional, not a
    // failure.
    void readReturnsEmptyOptionalWhenBackendReturnsNoBytes()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->scriptedResponse = QByteArray(); // empty

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(result.has_value());
        QVERIFY(!result->has_value());
    }

    // write()'s catch(const std::exception&) branch.
    void writeFailsWithInternalWhenDriverThrowsStandardException()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->throwOnWrite = true;

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const bytes::Bytes data{0xAA};
        const auto result = transport.write(bytes::ByteView(data), cancellation);

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
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->throwNonStandardOnWrite = true;

        DesktopCanFlashTransport transport(std::move(serial));
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
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->throwOnRead = true;

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50, cancellation);

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
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->throwNonStandardOnRead = true;

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        const auto result = transport.read(50, cancellation);

        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Internal);
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
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->scriptedResponse = QByteArray("\xAA", 1);

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(2);
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
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->throwOnRead = true;

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(2);
        const auto result = transport.read(50, cancellation);

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
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false);
        fake->throwNonStandardOnRead = true;

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(2);
        const auto result = transport.read(50, cancellation);

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
                                                              fake = new FakeBackend();
                                                              fake->destroyed = &destroyed;
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation

        DesktopCanFlashTransport transport(std::move(serial));
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
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake, &destroyed]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              fake->destroyed = &destroyed;
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation

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
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // create backend before wiring gates
        fake->scriptedResponse = QByteArray("\xAA", 1);

        QSemaphore readEntered;
        QSemaphore continueRead;
        fake->readEntered = &readEntered;
        fake->continueRead = &continueRead;

        DesktopCanFlashTransport transport(std::move(serial));
        FakeCancellationToken cancellation;

        fastecu::Result<std::optional<bytes::Bytes>> inFlightResult;
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
        QVERIFY2(!readerFinished.load(), "request_unblock() must not interrupt an already in-flight read");

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

    void fakeBackendReportsScriptedPortListAndBattery()
    {
        FakeBackend *fake = nullptr;
        auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
                                                          [&fake]() -> SerialBackend *
                                                          {
                                                              fake = new FakeBackend();
                                                              return fake;
                                                          });
        serial->set_add_ssm_header(false); // forces backend creation
        fake->checkSerialPortsResult = QStringList{"op2-0", "op2-1"};
        fake->vbattResult = 11676;
        fake->takeCallLog();

        QCOMPARE(serial->check_serial_ports(), QStringList({"op2-0", "op2-1"}));
        QVERIFY(serial->set_serial_port("op2-1"));
        QCOMPARE(serial->read_vbatt(), 11676ul);
        QVERIFY(fake->takeCallLog().contains("cfg:set_serial_port:op2-1"));
    }
};

QTEST_GUILESS_MAIN(TestDesktopCanFlashTransport)
#include "desktop_can_flash_transport_test.moc"
