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

TEST(TestSerialDiagnosticLink, klineOpenResetsAppliesEverySetterThenOpens)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), ResetConnection());
        EXPECT_CALL(serial.Fake(), SetIsIso14230Connection(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddSsmHeader(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddIso9141Header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddIso14230Header(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(QString("10400"))).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetSerialPortParity(static_cast<std::uint8_t>(QSerialPort::NoParity)))
            .WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetKlineStartbyte(0xC0)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetKlineTesterId(0xF1)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetKlineTargetId(0x33)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIs29BitId(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(Return(QString("ttyUSB0")));
    }
    SerialDiagnosticLink link(serial.Get());
    ASSERT_TRUE(link.Open(KlineLinkConfig{.header = KlineHeader::kIso14230,
                                          .iso14230_connection = true,
                                          .baud = 10400,
                                          .start_byte = 0xC0,
                                          .tester_id = 0xF1,
                                          .target_id = 0x33})
                    .has_value());
}

TEST(TestSerialDiagnosticLink, canOpenResetsAppliesEverySetterThenOpens)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), ResetConnection());
        EXPECT_CALL(serial.Fake(), SetIsIso14230Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddSsmHeader(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddIso9141Header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddIso14230Header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsCanConnection(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIs29BitId(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetCanSpeed(QString("250000"))).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIso15765SourceAddress(0x7E0U)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIso15765DestinationAddress(0x7E8U)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(Return(QString("j2534")));
    }
    SerialDiagnosticLink link(serial.Get());
    ASSERT_TRUE(
        link
            .Open(CanLinkConfig{
                .iso15765 = false, .bitrate = 250000, .extended_id = true, .source_id = 0x7E0, .destination_id = 0x7E8})
            .has_value());
}

TEST(TestSerialDiagnosticLink, evenParityIsAppliedBeforeTheOpen)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(QString("1953"))).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetSerialPortParity(static_cast<std::uint8_t>(QSerialPort::EvenParity)))
            .WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(Return(QString("ttyUSB0")));
    }
    SerialDiagnosticLink link(serial.Get());
    ASSERT_TRUE(link.Open(KlineLinkConfig{.baud = 1953, .parity = Parity::kEven}).has_value());
}

TEST(TestSerialDiagnosticLink, failingSetterIsInvalidConfigAndStopsTheSequence)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), SetIsIso14230Connection(false)).WillOnce(Return(false));
    EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(::testing::_)).Times(0);
    EXPECT_CALL(serial.Fake(), OpenSerialPort()).Times(0);
    SerialDiagnosticLink link(serial.Get());
    const auto result = link.Open(KlineLinkConfig{});
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
}

TEST(TestSerialDiagnosticLink, emptyOpenedPortIsDisconnected)
{
    FakeBackedSerial serial;
    ON_CALL(serial.Fake(), SetIsIso14230Connection(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetAddSsmHeader(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetAddIso9141Header(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetAddIso14230Header(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetSerialPortBaudrate(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetKlineStartbyte(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetKlineTesterId(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetKlineTargetId(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetIsCanConnection(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetIsIso15765Connection(::testing::_)).WillByDefault(Return(true));
    ON_CALL(serial.Fake(), SetIs29BitId(::testing::_)).WillByDefault(Return(true));
    EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(Return(QString()));
    SerialDiagnosticLink link(serial.Get());
    const auto result = link.Open(KlineLinkConfig{});
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

TEST(TestSerialDiagnosticLink, setHeaderSetsAllThreeFlags)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), SetAddSsmHeader(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddIso9141Header(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddIso14230Header(false)).WillOnce(Return(true));
    }
    SerialDiagnosticLink link(serial.Get());
    ASSERT_TRUE(link.SetHeader(KlineHeader::kIso9141).has_value());
}

TEST(TestSerialDiagnosticLink, p1UsesTheJ2534IoctlOnOpenPort)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), GetUseOpenport2Adapter()).WillRepeatedly(Return(true));
    EXPECT_CALL(serial.Fake(), SetJ2534Ioctl(0x07, 35)).WillOnce(Return(kSerialSuccess));
    EXPECT_CALL(serial.Fake(), SetKlineTimings(::testing::_, ::testing::_)).Times(0);
    SerialDiagnosticLink link(serial.Get());
    ASSERT_TRUE(link.SetP1Max(35ms).has_value());
}

TEST(TestSerialDiagnosticLink, p1UsesKlineTimingsOnDirectSerial)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), GetUseOpenport2Adapter()).WillRepeatedly(Return(false));
    EXPECT_CALL(serial.Fake(), SetKlineTimings(0x01, 25)).WillOnce(Return(true));
    EXPECT_CALL(serial.Fake(), SetJ2534Ioctl(::testing::_, ::testing::_)).Times(0);
    SerialDiagnosticLink link(serial.Get());
    ASSERT_TRUE(link.SetP1Max(25ms).has_value());
}

TEST(TestSerialDiagnosticLink, initCallsPassBytesThrough)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), FiveBaudInit(QByteArray::fromHex("33"))).WillOnce(Return(QByteArray::fromHex("550808")));
    EXPECT_CALL(serial.Fake(), FastInit(QByteArray::fromHex("81"))).WillOnce(Return(kSerialError));
    SerialDiagnosticLink link(serial.Get());
    const auto response = link.FiveBaudInit(0x33);
    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->size(), std::size_t{3});
    const auto fast = link.FastInit(bytes::Bytes{0x81});
    ASSERT_TRUE(!fast.has_value());
    ASSERT_EQ(fast.error().kind, ErrorKind::kDisconnected);
}

TEST(TestSerialDiagnosticLink, writeIsEchoCheckedAndReadsSelectTheFacadeCall)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), WriteSerialDataEchoCheck(QByteArray::fromHex("0100")))
        .WillOnce(Return(QByteArray::fromHex("0100")));
    EXPECT_CALL(serial.Fake(), ReadSerialData(200)).WillOnce(Return(QByteArray::fromHex("4100")));
    EXPECT_CALL(serial.Fake(), ReadSerialObdData(200)).WillOnce(Return(QByteArray()));
    SerialDiagnosticLink link(serial.Get());
    FakeCancellationToken token;
    ASSERT_TRUE(link.Write(bytes::Bytes{0x01, 0x00}).has_value());
    const auto frame = link.Read(200ms, token);
    ASSERT_TRUE(frame.has_value() && frame->has_value());
    const auto& payload = *frame;
    ASSERT_TRUE(payload.has_value());
    ASSERT_EQ(payload->size(), std::size_t{2});
    const auto none = link.ReadObd(200ms, token);
    ASSERT_TRUE(none.has_value() && !none->has_value());
}

TEST(TestSerialDiagnosticLink, cancelledReadNeverReachesTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), ReadSerialData(::testing::_)).Times(0);
    SerialDiagnosticLink link(serial.Get());
    FakeCancellationToken token(true);
    const auto result = link.Read(200ms, token);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST(TestSerialDiagnosticLink, nullFacadeIsDisconnected)
{
    SerialDiagnosticLink link(nullptr);
    ASSERT_EQ(link.Open(KlineLinkConfig{}).error().kind, ErrorKind::kDisconnected);
    ASSERT_EQ(link.Write(bytes::Bytes{0x01}).error().kind, ErrorKind::kDisconnected);
    ASSERT_TRUE(!link.UsesJ2534());
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
