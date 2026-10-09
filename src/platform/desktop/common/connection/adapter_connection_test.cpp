#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/connection/adapter_connection.h"

#include <QCoreApplication>
#include <QSerialPort>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include <gmock/gmock.h>

#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"

using fastecu::desktop::connection::AdapterConnection;
using fastecu::desktop::connection::LogTransport;
using fastecu::desktop::connection::LogTransportFromText;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;

TEST(TestAdapterConnection, parsesTheToolbarTransportText)
{
    ASSERT_EQ(LogTransportFromText("CAN"), LogTransport::kCan);
    ASSERT_EQ(LogTransportFromText("iso15765"), LogTransport::kIso15765);
    ASSERT_EQ(LogTransportFromText("K-Line"), LogTransport::kKLine);
    ASSERT_EQ(LogTransportFromText("SSM"), LogTransport::kSsm);
    ASSERT_EQ(LogTransportFromText(""), LogTransport::kOther);
}

TEST(TestAdapterConnection, listsPortsFromTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), CheckSerialPorts()).WillOnce(Return(QStringList{"ttyUSB0 - FT232"}));
    AdapterConnection connection(*serial);
    ASSERT_EQ(connection.AvailablePorts(), QStringList{"ttyUSB0 - FT232"});
}

TEST(TestAdapterConnection, setInitialPortSetsTheBaudThenThePort)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(QString("4800"))).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetSerialPort(QString("/dev/ttyUSB0"))).WillOnce(Return(true));
    }
    AdapterConnection connection(*serial);
    connection.SetInitialPort("/dev/ttyUSB0", "4800");
}

TEST(TestAdapterConnection, selectPortReplacesThePortList)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), SetSerialPortList(QStringList{"ttyUSB0"})).WillOnce(Return(true));
    AdapterConnection connection(*serial);
    connection.SelectPort("ttyUSB0");
}

TEST(TestAdapterConnection, openAndIsOpenAskTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), OpenSerialPort()).WillOnce(Return(QString("ttyUSB0")));
    EXPECT_CALL(serial.Fake(), IsSerialPortOpen()).WillOnce(Return(false));
    AdapterConnection connection(*serial);
    ASSERT_EQ(connection.Open(), QString("ttyUSB0"));
    ASSERT_TRUE(!connection.IsOpen());
}

TEST(TestAdapterConnection, openedPortAsksTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), GetOpenedSerialPort()).WillOnce(Return(QString("ttyUSB0")));
    AdapterConnection connection(*serial);
    ASSERT_EQ(connection.OpenedPort(), QString("ttyUSB0"));
}

TEST(TestAdapterConnection, canTransportIsRawCanElevenBit)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsCanConnection(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIs29BitId(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetCanSpeed(QString("500000"))).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), ResetConnection());
    }
    AdapterConnection connection(*serial);
    connection.ApplyLogTransport(LogTransport::kCan, false);
}

// Pinned: log_transport_changed set 29-bit identifiers for iso15765.
TEST(TestAdapterConnection, iso15765TransportIsTwentyNineBit)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIs29BitId(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetCanSpeed(QString("500000"))).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), ResetConnection());
    }
    AdapterConnection connection(*serial);
    connection.ApplyLogTransport(LogTransport::kIso15765, false);
}

TEST(TestAdapterConnection, klineWithSsmRunsAtFourThousandEightHundred)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), ChangePortSpeed(QString("4800"))).WillOnce(Return(0));
        EXPECT_CALL(serial.Fake(), ResetConnection());
    }
    AdapterConnection connection(*serial);
    connection.ApplyLogTransport(LogTransport::kKLine, true);
}

TEST(TestAdapterConnection, klineWithoutSsmLeavesTheSpeedAlone)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), ChangePortSpeed(_)).Times(0);
    EXPECT_CALL(serial.Fake(), ResetConnection());
    AdapterConnection connection(*serial);
    connection.ApplyLogTransport(LogTransport::kKLine, false);
}

TEST(TestAdapterConnection, clearLinkFlagsClearsEveryFlagAndKeepsParity)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), ResetConnection());
        EXPECT_CALL(serial.Fake(), SetIsIso14230Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIs29BitId(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetAddIso14230Header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsCanConnection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(QString("4800"))).WillOnce(Return(true));
    }
    EXPECT_CALL(serial.Fake(), SetSerialPortParity(_)).Times(0);
    AdapterConnection connection(*serial);
    connection.ClearLinkFlags();
}

TEST(TestAdapterConnection, returnToIdleResetsBaudAndParityAndKeepsTheFlags)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.Fake(), ResetConnection());
        EXPECT_CALL(serial.Fake(), SetSerialPortBaudrate(QString("4800"))).WillOnce(Return(true));
        EXPECT_CALL(serial.Fake(), SetSerialPortParity(static_cast<std::uint8_t>(QSerialPort::NoParity)))
            .WillOnce(Return(true));
    }
    EXPECT_CALL(serial.Fake(), SetIsIso14230Connection(_)).Times(0);
    EXPECT_CALL(serial.Fake(), SetIs29BitId(_)).Times(0);
    EXPECT_CALL(serial.Fake(), SetAddIso14230Header(_)).Times(0);
    EXPECT_CALL(serial.Fake(), SetIsCanConnection(_)).Times(0);
    EXPECT_CALL(serial.Fake(), SetIsIso15765Connection(_)).Times(0);
    AdapterConnection connection(*serial);
    connection.ReturnToIdle();
}

TEST(TestAdapterConnection, setPortSpeedPassesTheBaudAsText)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), ChangePortSpeed(QString("4800"))).WillOnce(Return(0));
    AdapterConnection connection(*serial);
    connection.SetPortSpeed(4800);
}

TEST(TestAdapterConnection, batteryIsReadOnlyFromAnOpenPort)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), GetUseOpenport2Adapter()).WillOnce(Return(false)).WillOnce(Return(true));
    EXPECT_CALL(serial.Fake(), ReadVbatt()).WillOnce(Return(12500UL));
    AdapterConnection connection(*serial);
    ASSERT_TRUE(!connection.BatteryMillivolts().has_value());
    const std::optional<unsigned long> reading = connection.BatteryMillivolts();
    ASSERT_TRUE(reading.has_value());
    ASSERT_EQ(*reading, 12500UL);
}

TEST(TestAdapterConnection, waitForSourceWaitsOnTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.Fake(), WaitForSource());
    AdapterConnection connection(*serial);
    connection.WaitForSource();
}

TEST(TestAdapterConnection, forwardsFacadeStateChanges)
{
    FakeBackedSerial serial;
    AdapterConnection connection(*serial);
    fastecu::testing::SignalRecorder spy(&connection, &AdapterConnection::stateChanged);
    emit serial->stateChanged(QRemoteObjectReplica::Valid, QRemoteObjectReplica::Default);
    ASSERT_EQ(spy.Count(), 1U);
}

TEST(TestAdapterConnection, exposesTheSameFacade)
{
    FakeBackedSerial serial;
    AdapterConnection connection(*serial);
    ASSERT_EQ(&connection.Facade(), serial.Get());
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
