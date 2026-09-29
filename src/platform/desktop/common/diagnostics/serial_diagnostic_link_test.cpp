#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"

#include <QCoreApplication>
#include <QSerialPort>
#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/serial_facade_codes.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::diagnostics::CanLinkConfig;
using fastecu::diagnostics::KlineHeader;
using fastecu::diagnostics::KlineLinkConfig;
using fastecu::diagnostics::Parity;
using fastecu::diagnostics::SerialDiagnosticLink;
using ::testing::InSequence;
using ::testing::Return;
using namespace std::chrono_literals;

class TestSerialDiagnosticLink : public ::testing::Test
{

  public:
};

TEST_F(TestSerialDiagnosticLink, klineOpenResetsAppliesEverySetterThenOpens)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), reset_connection());
        EXPECT_CALL(serial.fake(), set_is_iso14230_connection(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_ssm_header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_iso9141_header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_iso14230_header(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("10400"))).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)))
            .WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_kline_startbyte(0xC0)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_kline_tester_id(0xF1)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_kline_target_id(0x33)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("ttyUSB0")));
    }
    SerialDiagnosticLink link(serial.get());
    ASSERT_TRUE(link.open(KlineLinkConfig{.header = KlineHeader::Iso14230,
                                          .iso14230_connection = true,
                                          .baud = 10400,
                                          .start_byte = 0xC0,
                                          .tester_id = 0xF1,
                                          .target_id = 0x33})
                    .has_value());
}

TEST_F(TestSerialDiagnosticLink, canOpenResetsAppliesEverySetterThenOpens)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), reset_connection());
        EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_ssm_header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_iso9141_header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_can_connection(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_29_bit_id(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_can_speed(QString("250000"))).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_iso15765_source_address(0x7E0U)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_iso15765_destination_address(0x7E8U)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("j2534")));
    }
    SerialDiagnosticLink link(serial.get());
    ASSERT_TRUE(
        link
            .open(CanLinkConfig{
                .iso15765 = false, .bitrate = 250000, .extended_id = true, .source_id = 0x7E0, .destination_id = 0x7E8})
            .has_value());
}

TEST_F(TestSerialDiagnosticLink, evenParityIsAppliedBeforeTheOpen)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("1953"))).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::EvenParity)))
            .WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("ttyUSB0")));
    }
    SerialDiagnosticLink link(serial.get());
    ASSERT_TRUE(link.open(KlineLinkConfig{.baud = 1953, .parity = Parity::Even}).has_value());
}

TEST_F(TestSerialDiagnosticLink, failingSetterIsInvalidConfigAndStopsTheSequence)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(Return(false));
    EXPECT_CALL(serial.fake(), set_serial_port_baudrate(::testing::_)).Times(0);
    EXPECT_CALL(serial.fake(), open_serial_port()).Times(0);
    SerialDiagnosticLink link(serial.get());
    const auto result = link.open(KlineLinkConfig{});
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST_F(TestSerialDiagnosticLink, emptyOpenedPortIsDisconnected)
{
    FakeBackedSerial serial;
    ON_CALL(serial.fake(), set_is_iso14230_connection(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_add_ssm_header(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_add_iso9141_header(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_add_iso14230_header(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_serial_port_baudrate(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_kline_startbyte(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_kline_tester_id(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_kline_target_id(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_is_can_connection(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_is_iso15765_connection(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.fake(), set_is_29_bit_id(::testing::_)).WillByDefault(Return(true));
    EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString()));
    SerialDiagnosticLink link(serial.get());
    const auto result = link.open(KlineLinkConfig{});
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestSerialDiagnosticLink, setHeaderSetsAllThreeFlags)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), set_add_ssm_header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_iso9141_header(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(Return(true));
    }
    SerialDiagnosticLink link(serial.get());
    ASSERT_TRUE(link.set_header(KlineHeader::Iso9141).has_value());
}

TEST_F(TestSerialDiagnosticLink, p1UsesTheJ2534IoctlOnOpenPort)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillRepeatedly(Return(true));
    EXPECT_CALL(serial.fake(), set_j2534_ioctl(kJ2534IoctlP1Max, 35)).WillOnce(Return(STATUS_SUCCESS));
    EXPECT_CALL(serial.fake(), set_kline_timings(::testing::_, ::testing::_)).Times(0);
    SerialDiagnosticLink link(serial.get());
    ASSERT_TRUE(link.set_p1_max(35ms).has_value());
}

TEST_F(TestSerialDiagnosticLink, p1UsesKlineTimingsOnDirectSerial)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillRepeatedly(Return(false));
    EXPECT_CALL(serial.fake(), set_kline_timings(SERIAL_P1_MAX, 25)).WillOnce(Return(true));
    EXPECT_CALL(serial.fake(), set_j2534_ioctl(::testing::_, ::testing::_)).Times(0);
    SerialDiagnosticLink link(serial.get());
    ASSERT_TRUE(link.set_p1_max(25ms).has_value());
}

TEST_F(TestSerialDiagnosticLink, initCallsPassBytesThrough)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), five_baud_init(QByteArray::fromHex("33")))
        .WillOnce(Return(QByteArray::fromHex("550808")));
    EXPECT_CALL(serial.fake(), fast_init(QByteArray::fromHex("81"))).WillOnce(Return(STATUS_ERROR));
    SerialDiagnosticLink link(serial.get());
    const auto response = link.five_baud_init(0x33);
    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->size(), std::size_t{3});
    const auto fast = link.fast_init(bytes::Bytes{0x81});
    ASSERT_TRUE(!fast.has_value());
    ASSERT_EQ(fast.error().kind, ErrorKind::Disconnected);
}

TEST_F(TestSerialDiagnosticLink, writeIsEchoCheckedAndReadsSelectTheFacadeCall)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), write_serial_data_echo_check(QByteArray::fromHex("0100")))
        .WillOnce(Return(QByteArray::fromHex("0100")));
    EXPECT_CALL(serial.fake(), read_serial_data(200)).WillOnce(Return(QByteArray::fromHex("4100")));
    EXPECT_CALL(serial.fake(), read_serial_obd_data(200)).WillOnce(Return(QByteArray()));
    SerialDiagnosticLink link(serial.get());
    FakeCancellationToken token;
    ASSERT_TRUE(link.write(bytes::Bytes{0x01, 0x00}).has_value());
    const auto frame = link.read(200ms, token);
    ASSERT_TRUE(frame.has_value() && frame->has_value());
    ASSERT_EQ((*frame)->size(), std::size_t{2});
    const auto none = link.read_obd(200ms, token);
    ASSERT_TRUE(none.has_value() && !none->has_value());
}

TEST_F(TestSerialDiagnosticLink, cancelledReadNeverReachesTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), read_serial_data(::testing::_)).Times(0);
    SerialDiagnosticLink link(serial.get());
    FakeCancellationToken token(true);
    const auto result = link.read(200ms, token);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_F(TestSerialDiagnosticLink, nullFacadeIsDisconnected)
{
    SerialDiagnosticLink link(nullptr);
    ASSERT_EQ(link.open(KlineLinkConfig{}).error().kind, ErrorKind::Disconnected);
    ASSERT_EQ(link.write(bytes::Bytes{0x01}).error().kind, ErrorKind::Disconnected);
    ASSERT_TRUE(!link.uses_j2534());
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
    return RUN_ALL_TESTS();
}
