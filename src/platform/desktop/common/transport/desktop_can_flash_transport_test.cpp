#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <string>
#include "src/platform/desktop/common/testing/event_helpers.h"
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
#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"
#include "src/platform/desktop/common/transport/setter_sequence_expectations.h"

using namespace std::chrono_literals;

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::flash::DesktopCanFlashTransport;
using fastecu::flash::Iso15765Config;

class TestDesktopCanFlashTransport : public ::testing::Test
{

  public:
    // Data-driven sibling of configureChecksEveryBooleanSetterInOrderAndStops-
    // AtFirstFailure() above (which only exercises the fifth setter's
    // failure branch, set_can_speed): proves every remaining setter's own
    // InvalidConfig return path independently.

    // Success mirror of configureChecksEveryBooleanSetterInOrderAndStopsAt-
    // FirstFailure() above: every setter is expected to succeed, so
    // configure() must run all ten setters, in order, and return success.

    // A K-Line session on the same facade leaves ISO-14230 auto-headers on.
    // configure() must clear that state, or the first CAN frame goes out with
    // a K-Line header attached.

    // Success mirror of openFailureReturnsDisconnectedWithoutAnyWrite().

    // reset_connection() is the diesel startup seam. This calls the real
    // SerialPortActions facade over the existing fake backend rather than
    // asserting on a transport double.

    // write() success path: port open throughout, cancellation never fires.

    // write() must observe cancellation.cancelled() before ever issuing the
    // write -- the backend must never be touched at all. Distinct from the
    // K-Line sibling, whose write() takes no cancellation token at all.

    // write() disconnected-during path: the port closes as a side effect of
    // the write call itself -- the post-write is_serial_port_open() check
    // must catch this even though write_serial_data_echo_check() itself
    // never signals failure via its return value.

    // write() disconnected-before path: the port is already closed when
    // write() is called -- write_serial_data_echo_check() must never be
    // reached.

    // read() success path: port open, cancellation never fires, backend
    // returns scripted bytes.

    // read() observes cancellation.cancelled() before ever issuing the read
    // -- the backend must never be touched at all.

    // read() disconnected-before path: the port is already closed when
    // read() is called -- read_serial_data() must never be reached.

    // read() disconnected-during path: the read returns data, then the
    // post-read is_serial_port_open() check reports the closed port.

    // The "already closed" guard at the top of every method: once close()
    // has run, serial_ is null and every subsequent call must fail with
    // Disconnected without touching the (now possibly destroyed) backend.

    // write() must be skipped once request_unblock() has fired, exactly
    // like read() -- the shared unblock_requested_ flag guards both.

    // read() success path when the backend legitimately has nothing to
    // report: raw.isEmpty() must map to a present-but-empty optional, not a
    // failure.

    // write()'s catch(const std::exception&) branch.

    // write()'s bare catch(...) branch.

    // read()'s catch(const std::exception&) branch, with cancellation never
    // observed -- must map to Internal, not Cancelled.

    // read()'s bare catch(...) branch, with cancellation never observed.

    // read()'s post-read cancellation recheck (success path): cancellation
    // becomes observed-true only *after* the backend call has already
    // returned successfully -- must still map to Cancelled, not the bytes
    // that were read.

    // read()'s post-throw cancellation recheck, catch(const std::exception&)
    // branch: cancellation becomes observed-true only after the backend
    // call has already thrown -- must map to Cancelled, not Internal.

    // read()'s post-throw cancellation recheck, bare catch(...) branch.

    // Proves the non-owning constructor (step 5c, Task 17) -- see
    // test_desktop_kline_flash_transport.cpp's identically-named test for
    // the full rationale: MainWindow's single, session-lifetime
    // SerialPortActions instance must survive close() on a transport that
    // does not own it.

    // request_unblock() has no real interrupt primitive to fire --
    // SerialPortActions exposes none -- so it can only set a flag checked
    // before the *next* read call. This test proves both halves of that
    // documented, bounded-latency contract: (1) an already in-flight read
    // does NOT return early just because request_unblock() fires -- it
    // still returns only via its own existing timeout (simulated here by
    // releasing the fake's continueRead gate); and (2) once
    // request_unblock() has fired, the *next* call to read() returns
    // immediately as Cancelled without ever reaching the backend.
};

TEST_F(TestDesktopCanFlashTransport, configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure)
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

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

struct configureFailsAtEachRemainingSetterInTurnCase
{
    std::string name;
    int setterIndex;
};
class configureFailsAtEachRemainingSetterInTurnParameters
    : public TestDesktopCanFlashTransport,
      public ::testing::WithParamInterface<configureFailsAtEachRemainingSetterInTurnCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, configureFailsAtEachRemainingSetterInTurnParameters,
    ::testing::Values(configureFailsAtEachRemainingSetterInTurnCase{"set_is_iso15765_connection", 0},
                      configureFailsAtEachRemainingSetterInTurnCase{"set_is_can_connection", 1},
                      configureFailsAtEachRemainingSetterInTurnCase{"set_is_iso14230_connection", 2},
                      configureFailsAtEachRemainingSetterInTurnCase{"set_is_29_bit_id", 3},
                      configureFailsAtEachRemainingSetterInTurnCase{"set_can_source_address", 5},
                      configureFailsAtEachRemainingSetterInTurnCase{"set_can_destination_address", 6},
                      configureFailsAtEachRemainingSetterInTurnCase{"set_iso15765_source_address", 7},
                      configureFailsAtEachRemainingSetterInTurnCase{"set_iso15765_destination_address", 8},
                      configureFailsAtEachRemainingSetterInTurnCase{"set_add_iso14230_header", 9}),
    [](const ::testing::TestParamInfo<configureFailsAtEachRemainingSetterInTurnCase>& info)
    { return info.param.name; });

TEST_P(configureFailsAtEachRemainingSetterInTurnParameters, configureFailsAtEachRemainingSetterInTurn)
{
    const int setterIndex = GetParam().setterIndex;

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

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST_F(TestDesktopCanFlashTransport, openFailureReturnsDisconnectedWithoutAnyWrite)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(::testing::Return(QString{}));
    EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).Times(0);

    DesktopCanFlashTransport transport(serial.release());
    const auto result = transport.open();

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestDesktopCanFlashTransport, configureSucceedsWhenEverySetterSucceeds)
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

    ASSERT_TRUE(result.has_value());
}

TEST_F(TestDesktopCanFlashTransport, configureClearsStickyIso14230HeaderState)
{
    FakeBackedSerial serial;
    ASSERT_TRUE(serial->set_add_iso14230_header(true));
    SerialPortActions *observed = serial.get();

    DesktopCanFlashTransport transport(serial.release());
    const auto result = transport.configure(
        Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(observed->get_add_iso14230_header(), false);
}

TEST_F(TestDesktopCanFlashTransport, openSucceedsWhenBackendReturnsANonEmptyPortName)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(::testing::Return(QStringLiteral("COM3")));

    DesktopCanFlashTransport transport(serial.release());
    const auto result = transport.open();

    ASSERT_TRUE(result.has_value());
}

TEST_F(TestDesktopCanFlashTransport, resetConnectionSucceedsAndReachesTheAdapter)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), reset_connection()).WillOnce(::testing::Return());

    DesktopCanFlashTransport transport(serial.release());
    const auto result = transport.reset_connection();

    ASSERT_TRUE(result.has_value());
}

TEST_F(TestDesktopCanFlashTransport, restartIso15765ResetsConfiguresAndReopensInOrder)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.fake(), reset_connection()).WillOnce(::testing::Return());
    EXPECT_CALL(serial.fake(), set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_can_source_address(2017)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_can_destination_address(2025)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_iso15765_source_address(2017)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_iso15765_destination_address(2025)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(::testing::Return(QStringLiteral("COM3")));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.restart_iso15765(
        {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

    ASSERT_TRUE(result.has_value());
}

TEST_F(TestDesktopCanFlashTransport, restartIso15765CancellationBeforeResetTouchesNoBackendOperation)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), reset_connection()).Times(0);
    EXPECT_CALL(serial.fake(), set_is_iso15765_connection(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_is_can_connection(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_is_iso14230_connection(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_is_29_bit_id(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_can_speed(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_can_source_address(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_can_destination_address(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_iso15765_source_address(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_iso15765_destination_address(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), set_add_iso14230_header(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), open_serial_port()).Times(0);

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation(true);
    const auto result = transport.restart_iso15765(
        {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestDesktopCanFlashTransport, restartIso15765ResetFailureStopsBeforeConfigurationOrOpen)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), reset_connection())
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend reset failure")));
    EXPECT_CALL(serial.fake(), set_is_iso15765_connection(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), open_serial_port()).Times(0);

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.restart_iso15765(
        {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST_F(TestDesktopCanFlashTransport, restartIso15765ConfigureFailureStopsBeforeOpen)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.fake(), reset_connection()).WillOnce(::testing::Return());
    EXPECT_CALL(serial.fake(), set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial.fake(), open_serial_port()).Times(0);

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.restart_iso15765(
        {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST_F(TestDesktopCanFlashTransport, restartIso15765OpenFailurePropagatesAfterExactConfiguration)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.fake(), reset_connection()).WillOnce(::testing::Return());
    EXPECT_CALL(serial.fake(), set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_can_source_address(2017)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_can_destination_address(2025)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_iso15765_source_address(2017)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_iso15765_destination_address(2025)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(::testing::Return(QString{}));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.restart_iso15765(
        {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestDesktopCanFlashTransport, resetConnectionReturnsDisconnectedAfterClose)
{
    FakeBackedSerial serial;

    DesktopCanFlashTransport transport(serial.get()); // non-owning
    ASSERT_TRUE(transport.close().has_value());
    const auto result = transport.reset_connection();

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestDesktopCanFlashTransport, resetConnectionMapsStandardDriverExceptionsToInternal)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), reset_connection())
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend reset failure")));

    DesktopCanFlashTransport transport(serial.release());
    const auto result = transport.reset_connection();

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST_F(TestDesktopCanFlashTransport, resetConnectionMapsNonStandardDriverExceptionsToInternal)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), reset_connection()).WillOnce(ThrowNonStandardBackendFailure());

    DesktopCanFlashTransport transport(serial.release());
    const auto result = transport.reset_connection();

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST_F(TestDesktopCanFlashTransport, writeSucceedsWhenPortStaysOpenThroughout)
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

    ASSERT_TRUE(result.has_value());
}

TEST_F(TestDesktopCanFlashTransport, writeReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingWrite)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).Times(0);

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation(true);
    const bytes::Bytes data{0xAA};
    const auto result = transport.write(bytes::ByteView(data), cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestDesktopCanFlashTransport, writeFailsWithDisconnectedWhenPortClosesDuringWrite)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).WillOnce(::testing::Return(QByteArray{}));
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const bytes::Bytes data{0xAA};
    const auto result = transport.write(bytes::ByteView(data), cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestDesktopCanFlashTransport, writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).Times(0);

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const bytes::Bytes data{0xAA};
    const auto result = transport.write(bytes::ByteView(data), cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestDesktopCanFlashTransport, readReturnsScriptedBytesOnSuccess)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\x01\x02", 2)));
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    ASSERT_TRUE(result->value() == (bytes::Bytes{0x01, 0x02}));
}

TEST_F(TestDesktopCanFlashTransport, readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), read_serial_data(::testing::_)).Times(0);

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation(true);
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestDesktopCanFlashTransport, readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial.fake(), read_serial_data(::testing::_)).Times(0);

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestDesktopCanFlashTransport, readReturnsDisconnectedWhenPortClosesDuringRead)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\xAA", 1)));
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestDesktopCanFlashTransport, everyMethodFailsWithDisconnectedAfterClose)
{
    FakeBackedSerial serial;

    DesktopCanFlashTransport transport(serial.get()); // non-owning: keep `serial` alive
    auto closeResult = transport.close();
    ASSERT_TRUE(closeResult.has_value());

    FakeCancellationToken cancellation;
    const auto configureResult = transport.configure(
        Iso15765Config{.bitrate = 500000, .request_id = 0x7E0, .response_id = 0x7E8, .extended_id = false});
    ASSERT_TRUE(!configureResult.has_value());
    ASSERT_EQ(configureResult.error().kind, ErrorKind::Disconnected);

    const auto openResult = transport.open();
    ASSERT_TRUE(!openResult.has_value());
    ASSERT_EQ(openResult.error().kind, ErrorKind::Disconnected);

    const bytes::Bytes data{0xAA};
    const auto writeResult = transport.write(bytes::ByteView(data), cancellation);
    ASSERT_TRUE(!writeResult.has_value());
    ASSERT_EQ(writeResult.error().kind, ErrorKind::Disconnected);

    const auto readResult = transport.read(50ms, cancellation);
    ASSERT_TRUE(!readResult.has_value());
    ASSERT_EQ(readResult.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestDesktopCanFlashTransport, writeIsSkippedWithCancelledAfterRequestUnblock)
{
    FakeBackedSerial serial;

    DesktopCanFlashTransport transport(serial.release());
    transport.request_unblock();
    EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).Times(0);

    FakeCancellationToken cancellation;
    const bytes::Bytes data{0xAA};
    const auto result = transport.write(bytes::ByteView(data), cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestDesktopCanFlashTransport, readReturnsEmptyOptionalWhenBackendReturnsNoBytes)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(::testing::Return(QByteArray{}));
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(!result->has_value());
}

TEST_F(TestDesktopCanFlashTransport, writeFailsWithInternalWhenDriverThrowsStandardException)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const bytes::Bytes data{0xAA};
    const auto result = transport.write(bytes::ByteView(data), cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST_F(TestDesktopCanFlashTransport, writeFailsWithInternalWhenDriverThrowsNonStandardException)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), write_serial_data_echo_check(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const bytes::Bytes data{0xAA};
    const auto result = transport.write(bytes::ByteView(data), cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST_F(TestDesktopCanFlashTransport, readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), read_serial_data(50))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST_F(TestDesktopCanFlashTransport, readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(ThrowNonStandardBackendFailure());

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST_F(TestDesktopCanFlashTransport, readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(::testing::Return(QByteArray("\xAA", 1)));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(2);
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestDesktopCanFlashTransport, readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), read_serial_data(50))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(2);
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestDesktopCanFlashTransport,
       readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.fake(), read_serial_data(50)).WillOnce(ThrowNonStandardBackendFailure());

    DesktopCanFlashTransport transport(serial.release());
    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(2);
    const auto result = transport.read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestDesktopCanFlashTransport, closeIsIdempotentAndDestroysTheOwnedSerialPortActions)
{
    bool destroyed = false;
    FakeBackedSerial serial{[&destroyed](auto& fake) { fake.destroyed = &destroyed; }};

    DesktopCanFlashTransport transport(serial.release());
    ASSERT_TRUE(!destroyed);

    auto closeResult = transport.close();
    ASSERT_TRUE(closeResult.has_value());
    // ~SerialPortActions() deletes its backend via a
    // Qt::BlockingQueuedConnection (serial_backend_host.cpp), so by the
    // time close() returns, the fake is already gone.
    ASSERT_TRUE(destroyed);

    // Idempotent: calling again with an already-null serial_ must not crash.
    closeResult = transport.close();
    ASSERT_TRUE(closeResult.has_value());
}

TEST_F(TestDesktopCanFlashTransport, closeOnANonOwningSerialPortActionsDoesNotDestroyIt)
{
    bool destroyed = false;
    FakeBackedSerial serial{[&destroyed](auto& fake) { fake.destroyed = &destroyed; }};

    {
        DesktopCanFlashTransport transport(serial.get()); // non-owning
        ASSERT_TRUE(!destroyed);

        auto closeResult = transport.close();
        ASSERT_TRUE(closeResult.has_value());
        // The proof this test exists for: close() on a non-owning
        // transport must NOT destroy the externally-owned
        // SerialPortActions.
        ASSERT_TRUE(!destroyed);

        // Idempotent, same as the owning path.
        closeResult = transport.close();
        ASSERT_TRUE(closeResult.has_value());
        ASSERT_TRUE(!destroyed);
    }
    // transport is gone now; `serial` must still be alive and usable.
    ASSERT_TRUE(!destroyed);
    const bool stillCallable = serial->is_serial_port_open(); // must not crash
    Q_UNUSED(stillCallable);
    serial.reset(); // only now does the real teardown happen
    ASSERT_TRUE(destroyed);
}

TEST_F(TestDesktopCanFlashTransport, requestUnblockCausesAPendingReadToReturnPromptly)
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
    ASSERT_TRUE(readEntered.tryAcquire(1, 1000)) << "backend read did not start";

    transport.request_unblock();
    fastecu::testing::process_events_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(!readerFinished.load()) << "request_unblock() must not interrupt an already in-flight read";

    continueRead.release(); // simulates the backend's own bounded timeout firing
    reader.join();

    ASSERT_TRUE(readerFinished.load());
    ASSERT_TRUE(inFlightResult.has_value());
    ASSERT_TRUE(inFlightResult->has_value());
    ASSERT_TRUE(inFlightResult->value() == bytes::Bytes{0xAA});

    // Second half of the contract: the *next* read must not reach the
    // backend at all.
    EXPECT_CALL(serial.fake(), read_serial_data(::testing::_)).Times(0);
    const auto secondResult = transport.read(50ms, cancellation);
    ASSERT_TRUE(!secondResult.has_value());
    ASSERT_EQ(secondResult.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestDesktopCanFlashTransport, fakeBackendReportsScriptedPortListAndBattery)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), check_serial_ports()).WillOnce(::testing::Return(QStringList{"op2-0", "op2-1"}));
    // DoDefault() rather than Return(true): the selected port must actually
    // reach the backend's configuration state, which Return(true) would skip.
    EXPECT_CALL(serial.fake(), set_serial_port(QStringLiteral("op2-1"))).WillOnce(::testing::DoDefault());
    EXPECT_CALL(serial.fake(), read_vbatt()).WillOnce(::testing::Return(11676UL));

    ASSERT_EQ(serial->check_serial_ports(), QStringList({"op2-0", "op2-1"}));
    ASSERT_TRUE(serial->set_serial_port("op2-1"));
    ASSERT_EQ(serial->get_serial_port(), QStringLiteral("op2-1"));
    ASSERT_EQ(serial->read_vbatt(), 11676UL);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
    return RUN_ALL_TESTS();
}
