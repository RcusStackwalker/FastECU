#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <string>
#include "src/platform/desktop/common/testing/event_helpers.h"
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
#include <gtest/gtest.h>
#include <QSerialPort>

#include <gmock/gmock.h>

#include <atomic>
#include <stdexcept>
#include <thread>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"
#include "src/platform/desktop/common/transport/setter_sequence_expectations.h"

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::flash::DesktopKlineFlashTransport;
using fastecu::flash::KlineConfig;
using fastecu::flash::KlineParity;
using namespace std::chrono_literals;

TEST(TestDesktopKlineFlashTransport, configureSetsAndResetsParityOnReusedFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), SetSerialPortParity(static_cast<std::uint8_t>(QSerialPort::EvenParity)))
        .WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), SetSerialPortParity(static_cast<std::uint8_t>(QSerialPort::NoParity)))
        .WillOnce(::testing::Return(true));
    DesktopKlineFlashTransport transport(serial.Release());
    KlineConfig config{.baud = 1953, .iso14230 = false, .tester_id = 0, .target_id = 0};
    config.parity = KlineParity::kEven;
    ASSERT_TRUE(transport.Configure(config).has_value());
    config.parity = KlineParity::kNone;
    ASSERT_TRUE(transport.Configure(config).has_value());
}

TEST(TestDesktopKlineFlashTransport, configureReportsParitySetterFailure)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), SetSerialPortParity(static_cast<std::uint8_t>(QSerialPort::OddParity)))
        .WillOnce(::testing::Return(false));
    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.Configure(
        KlineConfig{.baud = 1953, .iso14230 = false, .tester_id = 0, .target_id = 0, .parity = KlineParity::kOdd});
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
}

TEST(TestDesktopKlineFlashTransport, rawCallsUseRawSerialMethods)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillRepeatedly(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), WriteSerialData(QByteArray::fromHex("aabb"))).Times(1);
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(::testing::_)).Times(0);
    EXPECT_CALL(serial.Fake(), ReadSerialObdData(10)).WillOnce(::testing::Return(QByteArray::fromHex("00ff")));
    EXPECT_CALL(serial.Fake(), ReadSerialData(::testing::_)).Times(0);
    DesktopKlineFlashTransport transport(serial.Release());
    const bytes::Bytes request{0xaa, 0xbb};
    ASSERT_TRUE(transport.WriteRaw(request).has_value());
    FakeCancellationToken cancellation;
    const auto result = transport.ReadRaw(10ms, cancellation);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    ASSERT_EQ(result->value(), (bytes::Bytes{0x00, 0xff}));
}

TEST(TestDesktopKlineFlashTransport, postKernelUploadDelayCapabilityMirrorsOpenPort2OnUnix)
{
    FakeBackedSerial serial;
    SerialPortActions *serial_ptr = serial.Get();
    DesktopKlineFlashTransport transport(serial.Release());

    ASSERT_TRUE(!transport.RequiresPostKernelUploadDelay());
    ASSERT_TRUE(serial_ptr->SetUseOpenport2Adapter(true));
#if defined(Q_OS_UNIX)
    ASSERT_TRUE(transport.RequiresPostKernelUploadDelay());
#else
    ASSERT_TRUE(!transport.RequiresPostKernelUploadDelay());
#endif
}

TEST(TestDesktopKlineFlashTransport, programmingVoltageSupplyMirrorsOpenPort2OnEveryPlatform)
{
    FakeBackedSerial serial;
    SerialPortActions *serial_ptr = serial.Get();
    ASSERT_TRUE(!fastecu::flash::AdapterSuppliesProgrammingVoltage(nullptr));
    ASSERT_TRUE(!fastecu::flash::AdapterSuppliesProgrammingVoltage(serial_ptr));
    ASSERT_TRUE(serial_ptr->SetUseOpenport2Adapter(true));
    ASSERT_TRUE(fastecu::flash::AdapterSuppliesProgrammingVoltage(serial_ptr));
}

// reset_connection() is the SH705x K-Line startup seam. It calls the real
// SerialPortActions facade over FakeBackend, as the CAN adapter test does.
TEST(TestDesktopKlineFlashTransport, resetConnectionReachesTheAdapter)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), ResetConnection()).WillOnce(::testing::Return());

    DesktopKlineFlashTransport transport(serial.Release());

    ASSERT_TRUE(transport.ResetConnection().has_value());
}

TEST(TestDesktopKlineFlashTransport, resetConnectionAfterCloseIsDisconnectedAndTouchesNoBackend)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), ResetConnection()).Times(0);

    DesktopKlineFlashTransport transport(serial.Get()); // non-owning: keep `serial` alive
    ASSERT_TRUE(transport.Close().has_value());
    const auto result = transport.ResetConnection();

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

TEST(TestDesktopKlineFlashTransport, lecControlOperationsForwardToSerialBackend)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), SetLecLines(1, 1)).WillOnce(::testing::Return(kSerialSuccess));
    EXPECT_CALL(serial.Fake(), PulseLec2Line(200)).WillOnce(::testing::Return(kSerialSuccess));
    EXPECT_CALL(serial.Fake(), SetLecLines(0, 1)).WillOnce(::testing::Return(kSerialSuccess));
    EXPECT_CALL(serial.Fake(), SetLecLines(0, 0)).WillOnce(::testing::Return(kSerialSuccess));

    DesktopKlineFlashTransport transport(serial.Release());

    ASSERT_TRUE(transport.DisableLecLines().has_value());
    ASSERT_TRUE(transport.PulseLec2Line(200ms).has_value());
    ASSERT_TRUE(transport.EnableProgrammingVoltageLine().has_value());
    // Legacy bootmode execute() :66 -- VPP on LEC1 and MOD1 on LEC2.
    ASSERT_TRUE(transport.EnableBootModeLines().has_value());
}

TEST(TestDesktopKlineFlashTransport, configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure)
{
    FakeBackedSerial serial;
    // Fail the *third* setter in configure()'s specified order
    // (set_is_iso14230_connection, set_is_can_connection,
    // set_is_iso15765_connection, set_is_29_bit_id,
    // set_serial_port_baudrate) -- this proves both that the first two
    // setters really ran, in order, and that nothing after the failure
    // (the 29-bit-id setter, the baudrate setter, open()) ran.
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), SetIsIso14230Connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial.Fake(), SetIs29BitId(::testing::_)).Times(0);
    EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(::testing::_)).Times(0);
    EXPECT_CALL(serial.Fake(), OpenSerialPort()).Times(0);

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result =
        transport.Configure(KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
}

struct ConfigureFailsAtEachRemainingSetterInTurnCase
{
    std::string name;
    int setter_index;
};
class ConfigureFailsAtEachRemainingSetterInTurnParameters
    : public ::testing::Test,
      public ::testing::WithParamInterface<ConfigureFailsAtEachRemainingSetterInTurnCase>
{
};

INSTANTIATE_TEST_SUITE_P(
    Rows, ConfigureFailsAtEachRemainingSetterInTurnParameters,
    ::testing::Values(ConfigureFailsAtEachRemainingSetterInTurnCase{"set_is_iso14230_connection", 0},
                      ConfigureFailsAtEachRemainingSetterInTurnCase{"set_is_can_connection", 1},
                      ConfigureFailsAtEachRemainingSetterInTurnCase{"set_is_29_bit_id", 3},
                      ConfigureFailsAtEachRemainingSetterInTurnCase{"set_serial_port_baudrate", 4}),
    [](const ::testing::TestParamInfo<ConfigureFailsAtEachRemainingSetterInTurnCase>& info)
    { return info.param.name; });

// Data-driven sibling of configureChecksEveryBooleanSetterInOrderAndStops-
// AtFirstFailure() above (which only exercises the third setter's
// failure branch): proves every remaining setter's own InvalidConfig
// return path independently. (The third setter,
// set_is_iso15765_connection, is already covered by that test above, so
// it is intentionally omitted here.)
TEST_P(ConfigureFailsAtEachRemainingSetterInTurnParameters, configureFailsAtEachRemainingSetterInTurn)
{
    const int setter_index = GetParam().setter_index;

    FakeBackedSerial serial;

    ::testing::InSequence sequence;
    ExpectSetterAt(EXPECT_CALL(serial.Fake(), SetIsIso14230Connection(true)), 0, setter_index);
    ExpectSetterAt(EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)), 1, setter_index);
    ExpectSetterAt(EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)), 2, setter_index);
    ExpectSetterAt(EXPECT_CALL(serial.Fake(), SetIs29BitId(false)), 3, setter_index);
    ExpectSetterAt(EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(QStringLiteral("10400"))), 4, setter_index);
    EXPECT_CALL(serial.Fake(), OpenSerialPort()).Times(0);

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result =
        transport.Configure(KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
}

TEST(TestDesktopKlineFlashTransport, openFailureReturnsDisconnectedWithoutAnyWrite)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(::testing::Return(QString{}));
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(::testing::_)).Times(0);

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.Open();

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

// Success mirror of configureChecksEveryBooleanSetterInOrderAndStopsAt-
// FirstFailure() above: every setter is expected to succeed, so
// configure() must run all five setters, in order, and return success.
TEST(TestDesktopKlineFlashTransport, configureSucceedsWhenEverySetterSucceeds)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), SetIsIso14230Connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), SetIs29BitId(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(QStringLiteral("10400"))).WillOnce(::testing::Return(true));

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result =
        transport.Configure(KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});

    ASSERT_TRUE(result.has_value());
}

// Success mirror of openFailureReturnsDisconnectedWithoutAnyWrite().
TEST(TestDesktopKlineFlashTransport, openSucceedsWhenBackendReturnsANonEmptyPortName)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(::testing::Return(QStringLiteral("COM3")));

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.Open();

    ASSERT_TRUE(result.has_value());
}

// setBaud() success path: port open, change_port_speed() returns the
// real backend's success sentinel (STATUS_SUCCESS == 0, FakeBackend's
// default).
TEST(TestDesktopKlineFlashTransport, setBaudSucceedsWhenPortOpenAndDriverReturnsSuccess)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ChangePortSpeed(QStringLiteral("4800"))).WillOnce(::testing::Return(kSerialSuccess));

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.SetBaud(4800);

    ASSERT_TRUE(result.has_value());
}

// setBaud() failure path: port stays open, but change_port_speed()
// returns the real backend's failure sentinel (STATUS_ERROR, a small
// *positive* value) -- maps to Internal, not InvalidConfig (a runtime
// driver rejection, not a config-shape problem).
TEST(TestDesktopKlineFlashTransport, setBaudFailsWithInternalWhenPortStaysOpenButDriverRejectsChange)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ChangePortSpeed(QStringLiteral("4800"))).WillOnce(::testing::Return(kSerialError));
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.SetBaud(4800);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

// setBaud() disconnected-before path: the port is already closed when
// setBaud() is called -- change_port_speed() must never be reached.
TEST(TestDesktopKlineFlashTransport, setBaudFailsWithDisconnectedWhenPortAlreadyClosed)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial.Fake(), ChangePortSpeed(::testing::_)).Times(0);

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.SetBaud(4800);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

// setBaud() disconnected-during path: change_port_speed() itself reports
// a failure code (driver rejected/errored) AND the port is observed
// closed on the follow-up is_serial_port_open() check -- must map to
// Disconnected, not the generic Internal "driver rejected" branch.
TEST(TestDesktopKlineFlashTransport, setBaudFailsWithDisconnectedWhenPortClosesDuringBaudChange)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ChangePortSpeed(QStringLiteral("4800"))).WillOnce(::testing::Return(kSerialError));
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(false));

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.SetBaud(4800);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

// write() success path: port open throughout, echo-check write reports
// nothing useful in its return value (see the adapter's comment), so
// is_serial_port_open() staying true is the only real post-condition;
// success returns the number of bytes requested.
TEST(TestDesktopKlineFlashTransport, writeSucceedsAndReturnsRequestedByteCount)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(QByteArray::fromHex("010203")))
        .WillOnce(::testing::Return(QByteArray{}));
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));

    DesktopKlineFlashTransport transport(serial.Release());
    const bytes::Bytes data{0x01, 0x02, 0x03};
    const auto result = transport.Write(bytes::ByteView(data));

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, data.size());
}

// write() disconnected-during path: the port closes as a side effect of
// the write call itself (e.g. the adapter dropped mid-transfer) -- the
// post-write is_serial_port_open() check must catch this even though
// write_serial_data_echo_check() itself never signals failure via its
// return value.
TEST(TestDesktopKlineFlashTransport, writeFailsWithDisconnectedWhenPortClosesDuringWrite)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(::testing::_)).WillOnce(::testing::Return(QByteArray{}));
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(false));

    DesktopKlineFlashTransport transport(serial.Release());
    const bytes::Bytes data{0xAA};
    const auto result = transport.Write(bytes::ByteView(data));

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

// read() success path: port open, cancellation never fires, backend
// returns scripted bytes -- read() must return exactly those bytes.
TEST(TestDesktopKlineFlashTransport, readReturnsScriptedBytesOnSuccess)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(::testing::Return(QByteArray("\x01\x02", 2)));
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    ASSERT_TRUE(result->value() == (bytes::Bytes{0x01, 0x02}));
}

// read() observes cancellation.cancelled() before ever issuing the read
// -- the backend must never be touched at all.
TEST(TestDesktopKlineFlashTransport, readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), ReadSerialData(::testing::_)).Times(0);

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation(true);
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

// read() disconnected-before path: the port is already closed when
// read() is called -- read_serial_data() must never be reached.
TEST(TestDesktopKlineFlashTransport, readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial.Fake(), ReadSerialData(::testing::_)).Times(0);

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

// read() disconnected-during path: the read returns data, then the
// post-read is_serial_port_open() check reports the closed port.
TEST(TestDesktopKlineFlashTransport, readReturnsDisconnectedWhenPortClosesDuringRead)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(::testing::Return(QByteArray("\xAA", 1)));
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(false));

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

// The "already closed" guard at the top of every method: once close()
// has run, serial_ is null and every subsequent call must fail with
// Disconnected without touching the (now possibly destroyed) backend.
TEST(TestDesktopKlineFlashTransport, everyMethodFailsWithDisconnectedAfterClose)
{
    FakeBackedSerial serial;

    DesktopKlineFlashTransport transport(serial.Get()); // non-owning: keep `serial` alive
    auto close_result = transport.Close();
    ASSERT_TRUE(close_result.has_value());

    FakeCancellationToken cancellation;
    const auto configure_result =
        transport.Configure(KlineConfig{.baud = 10400, .iso14230 = true, .tester_id = 0x10, .target_id = 0xf0});
    ASSERT_TRUE(!configure_result.has_value());
    ASSERT_EQ(configure_result.error().kind, ErrorKind::kDisconnected);

    const auto open_result = transport.Open();
    ASSERT_TRUE(!open_result.has_value());
    ASSERT_EQ(open_result.error().kind, ErrorKind::kDisconnected);

    const auto set_baud_result = transport.SetBaud(4800);
    ASSERT_TRUE(!set_baud_result.has_value());
    ASSERT_EQ(set_baud_result.error().kind, ErrorKind::kDisconnected);

    const bytes::Bytes data{0xAA};
    const auto write_result = transport.Write(bytes::ByteView(data));
    ASSERT_TRUE(!write_result.has_value());
    ASSERT_EQ(write_result.error().kind, ErrorKind::kDisconnected);

    const auto read_result = transport.Read(50ms, cancellation);
    ASSERT_TRUE(!read_result.has_value());
    ASSERT_EQ(read_result.error().kind, ErrorKind::kDisconnected);

    const auto header_result = transport.SetAddIso14230Header(true);
    ASSERT_TRUE(!header_result.has_value());
    ASSERT_EQ(header_result.error().kind, ErrorKind::kDisconnected);

    const auto disable_lec_result = transport.DisableLecLines();
    ASSERT_TRUE(!disable_lec_result.has_value());
    ASSERT_EQ(disable_lec_result.error().kind, ErrorKind::kDisconnected);

    const auto pulse_lec_result = transport.PulseLec2Line(200ms);
    ASSERT_TRUE(!pulse_lec_result.has_value());
    ASSERT_EQ(pulse_lec_result.error().kind, ErrorKind::kDisconnected);

    const auto programming_line_result = transport.EnableProgrammingVoltageLine();
    ASSERT_TRUE(!programming_line_result.has_value());
    ASSERT_EQ(programming_line_result.error().kind, ErrorKind::kDisconnected);

    const auto boot_mode_lines_result = transport.EnableBootModeLines();
    ASSERT_TRUE(!boot_mode_lines_result.has_value());
    ASSERT_EQ(boot_mode_lines_result.error().kind, ErrorKind::kDisconnected);
}

// set_add_iso14230_header() forwards straight to
// SerialPortActions::set_add_iso14230_header() -- the seam
// DensoSh705xEepromKlineExecutor::execute() uses to turn the driver's
// auto-header on for read_mem()'s raw SID_DUMP requests and back off for
// connect_bootloader()/upload_kernel()'s self-framed exchanges. Verified
// through the real (non-owning) SerialPortActions, not just a mock call,
// so this actually proves the flag the driver reads changes.
TEST(TestDesktopKlineFlashTransport, setAddIso14230HeaderForwardsToSerialAndSucceeds)
{
    FakeBackedSerial serial;
    ASSERT_EQ(serial->GetAddIso14230Header(), false); // default

    DesktopKlineFlashTransport transport(serial.Get()); // non-owning: query `serial` after

    const auto on_result = transport.SetAddIso14230Header(true);
    ASSERT_TRUE(on_result.has_value());
    ASSERT_EQ(serial->GetAddIso14230Header(), true);

    const auto off_result = transport.SetAddIso14230Header(false);
    ASSERT_TRUE(off_result.has_value());
    ASSERT_EQ(serial->GetAddIso14230Header(), false);
}

// write() must be skipped once request_unblock() has fired, exactly
// like read() -- the shared unblock_requested_ flag guards both.
TEST(TestDesktopKlineFlashTransport, writeIsSkippedWithCancelledAfterRequestUnblock)
{
    FakeBackedSerial serial;

    DesktopKlineFlashTransport transport(serial.Release());
    transport.RequestUnblock();
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(::testing::_)).Times(0);

    const bytes::Bytes data{0xAA};
    const auto result = transport.Write(bytes::ByteView(data));

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

// write() disconnected-before path: the port is already closed when
// write() is called -- write_serial_data_echo_check() must never be
// reached. (Symmetric to the CAN sibling's identically-named test.)
TEST(TestDesktopKlineFlashTransport, writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(::testing::_)).Times(0);

    DesktopKlineFlashTransport transport(serial.Release());
    const bytes::Bytes data{0xAA};
    const auto result = transport.Write(bytes::ByteView(data));

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

// read() success path when the backend legitimately has nothing to
// report: raw.isEmpty() must map to a present-but-empty OptionalBytes,
// not a failure.
TEST(TestDesktopKlineFlashTransport, readReturnsEmptyOptionalWhenBackendReturnsNoBytes)
{
    FakeBackedSerial serial;
    ::testing::InSequence sequence;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(::testing::Return(QByteArray{}));
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(!result->has_value());
}

// setBaud()'s catch(const std::exception&) branch: change_port_speed()
// itself throws a standard exception -- must map to Internal.
TEST(TestDesktopKlineFlashTransport, setBaudFailsWithInternalWhenDriverThrowsStandardException)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ChangePortSpeed(QStringLiteral("4800")))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend baud-change failure")));

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.SetBaud(4800);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

// setBaud()'s bare catch(...) branch: a non-std::exception-derived
// failure must still be caught and mapped to Internal.
TEST(TestDesktopKlineFlashTransport, setBaudFailsWithInternalWhenDriverThrowsNonStandardException)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ChangePortSpeed(QStringLiteral("4800"))).WillOnce(ThrowNonStandardBackendFailure());

    DesktopKlineFlashTransport transport(serial.Release());
    const auto result = transport.SetBaud(4800);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

// write()'s catch(const std::exception&) branch.
TEST(TestDesktopKlineFlashTransport, writeFailsWithInternalWhenDriverThrowsStandardException)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(::testing::_))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend write failure")));

    DesktopKlineFlashTransport transport(serial.Release());
    const bytes::Bytes data{0xAA};
    const auto result = transport.Write(bytes::ByteView(data));

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

// write()'s bare catch(...) branch.
TEST(TestDesktopKlineFlashTransport, writeFailsWithInternalWhenDriverThrowsNonStandardException)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(::testing::_)).WillOnce(ThrowNonStandardBackendFailure());

    DesktopKlineFlashTransport transport(serial.Release());
    const bytes::Bytes data{0xAA};
    const auto result = transport.Write(bytes::ByteView(data));

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

// read()'s catch(const std::exception&) branch, with cancellation never
// observed -- must map to Internal, not Cancelled.
TEST(TestDesktopKlineFlashTransport, readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

// read()'s bare catch(...) branch, with cancellation never observed.
TEST(TestDesktopKlineFlashTransport, readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(ThrowNonStandardBackendFailure());

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInternal);
}

// isOpen(): true while the port is open, false once closed, false when
// the underlying check throws (caught, never propagated), and false
// once this transport itself has been closed (serial_ is null).
TEST(TestDesktopKlineFlashTransport, isOpenReflectsThePortsRealOpenState)
{
    FakeBackedSerial serial;

    DesktopKlineFlashTransport transport(serial.Release());
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true)).WillOnce(::testing::Return(false));
    ASSERT_TRUE(transport.IsOpen());

    ASSERT_TRUE(!transport.IsOpen());
}

TEST(TestDesktopKlineFlashTransport, isOpenReturnsFalseWhenTheUnderlyingCheckThrows)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen())
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend open-state failure")));

    DesktopKlineFlashTransport transport(serial.Release());
    ASSERT_TRUE(!transport.IsOpen());
}

TEST(TestDesktopKlineFlashTransport, isOpenReturnsFalseAfterClose)
{
    FakeBackedSerial serial;

    DesktopKlineFlashTransport transport(serial.Release());
    ASSERT_TRUE(transport.IsOpen());

    const auto close_result = transport.Close();
    ASSERT_TRUE(close_result.has_value());
    ASSERT_TRUE(!transport.IsOpen());
}

// read()'s post-read cancellation recheck (success path): cancellation
// becomes observed-true only *after* the backend call has already
// returned successfully -- must still map to Cancelled, not the bytes
// that were read.
TEST(TestDesktopKlineFlashTransport, readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(::testing::Return(QByteArray("\xAA", 1)));

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(2);
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

// read()'s post-throw cancellation recheck, catch(const std::exception&)
// branch: cancellation becomes observed-true only after the backend
// call has already thrown -- must map to Cancelled, not Internal.
TEST(TestDesktopKlineFlashTransport, readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50))
        .WillOnce(::testing::Throw(std::runtime_error("scripted backend read failure")));

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(2);
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

// read()'s post-throw cancellation recheck, bare catch(...) branch.
TEST(TestDesktopKlineFlashTransport,
     readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50)).WillOnce(ThrowNonStandardBackendFailure());

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(2);
    const auto result = transport.Read(50ms, cancellation);

    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST(TestDesktopKlineFlashTransport, closeIsIdempotentAndDestroysTheOwnedSerialPortActions)
{
    bool destroyed = false;
    FakeBackedSerial serial{[&destroyed](auto& fake) { fake.destroyed = &destroyed; }};

    DesktopKlineFlashTransport transport(serial.Release());
    ASSERT_TRUE(!destroyed);

    auto close_result = transport.Close();
    ASSERT_TRUE(close_result.has_value());
    // ~SerialPortActions() deletes its backend via a
    // Qt::BlockingQueuedConnection (serial_backend_host.cpp), so by the
    // time close() returns, the fake is already gone.
    ASSERT_TRUE(destroyed);

    // Idempotent: calling again with an already-null serial_ must not crash.
    close_result = transport.Close();
    ASSERT_TRUE(close_result.has_value());
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
// down. The local `serial` fixture (which holds the facade on this test's
// behalf, standing in for MainWindow's member) is then used again after
// close() to prove it is still a live, callable object, not a dangling
// pointer.
TEST(TestDesktopKlineFlashTransport, closeOnANonOwningSerialPortActionsDoesNotDestroyIt)
{
    bool destroyed = false;
    FakeBackedSerial serial{[&destroyed](auto& fake) { fake.destroyed = &destroyed; }};

    {
        DesktopKlineFlashTransport transport(serial.Get()); // non-owning
        ASSERT_TRUE(!destroyed);

        auto close_result = transport.Close();
        ASSERT_TRUE(close_result.has_value());
        // The proof this test exists for: close() on a non-owning
        // transport must NOT destroy the externally-owned
        // SerialPortActions.
        ASSERT_TRUE(!destroyed);

        // Idempotent, same as the owning path.
        close_result = transport.Close();
        ASSERT_TRUE(close_result.has_value());
        ASSERT_TRUE(!destroyed);
    }
    // transport is gone now; `serial` must still be alive and usable --
    // proves this isn't merely "destroyed wasn't set yet", but that the
    // object genuinely survives past the transport's own lifetime.
    ASSERT_TRUE(!destroyed);
    const bool still_callable = serial->IsSerialPortOpen(); // must not crash
    Q_UNUSED(still_callable);
    serial.Reset(); // only now does the real teardown happen
    ASSERT_TRUE(destroyed);
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
TEST(TestDesktopKlineFlashTransport, requestUnblockCausesAPendingReadToReturnPromptly)
{
    FakeBackedSerial serial;

    QSemaphore read_entered;
    QSemaphore continue_read;
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(serial.Fake(), ReadSerialData(50))
        .WillOnce(
            [&read_entered, &continue_read](std::uint16_t)
            {
                read_entered.release();
                continue_read.acquire();
                return QByteArray("\xAA", 1);
            });

    DesktopKlineFlashTransport transport(serial.Release());
    FakeCancellationToken cancellation;

    fastecu::Result<DesktopKlineFlashTransport::OptionalBytes> in_flight_result;
    std::atomic<bool> reader_finished{false};
    std::thread reader(
        [&]
        {
            in_flight_result = transport.Read(50ms, cancellation);
            reader_finished.store(true);
        });
    ASSERT_TRUE(read_entered.tryAcquire(1, 1000)) << "backend read did not start";

    transport.RequestUnblock();
    fastecu::testing::ProcessEventsFor(std::chrono::milliseconds(50));
    ASSERT_TRUE(!reader_finished.load()) << "request_unblock() must not interrupt an already in-flight read";

    continue_read.release(); // simulates the backend's own bounded timeout firing
    reader.join();

    ASSERT_TRUE(reader_finished.load());
    ASSERT_TRUE(in_flight_result.has_value());
    ASSERT_TRUE(in_flight_result->has_value());
    ASSERT_TRUE(in_flight_result->value() == bytes::Bytes{0xAA});

    // Second half of the contract: the *next* read must not reach the
    // backend at all.
    EXPECT_CALL(serial.Fake(), ReadSerialData(::testing::_)).Times(0);
    const auto second_result = transport.Read(50ms, cancellation);
    ASSERT_TRUE(!second_result.has_value());
    ASSERT_EQ(second_result.error().kind, ErrorKind::kCancelled);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
