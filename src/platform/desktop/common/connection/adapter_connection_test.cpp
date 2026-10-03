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
using fastecu::desktop::connection::log_transport_from_text;
using fastecu::desktop::connection::LogTransport;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;

TEST(TestAdapterConnection, parsesTheToolbarTransportText)
{
    ASSERT_EQ(log_transport_from_text("CAN"), LogTransport::Can);
    ASSERT_EQ(log_transport_from_text("iso15765"), LogTransport::Iso15765);
    ASSERT_EQ(log_transport_from_text("K-Line"), LogTransport::KLine);
    ASSERT_EQ(log_transport_from_text("SSM"), LogTransport::Ssm);
    ASSERT_EQ(log_transport_from_text(""), LogTransport::Other);
}

TEST(TestAdapterConnection, listsPortsFromTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), check_serial_ports()).WillOnce(Return(QStringList{"ttyUSB0 - FT232"}));
    AdapterConnection connection(*serial);
    ASSERT_EQ(connection.available_ports(), QStringList{"ttyUSB0 - FT232"});
}

TEST(TestAdapterConnection, setInitialPortSetsTheBaudThenThePort)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("4800"))).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_serial_port(QString("/dev/ttyUSB0"))).WillOnce(Return(true));
    }
    AdapterConnection connection(*serial);
    connection.set_initial_port("/dev/ttyUSB0", "4800");
}

TEST(TestAdapterConnection, selectPortReplacesThePortList)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), set_serial_port_list(QStringList{"ttyUSB0"})).WillOnce(Return(true));
    AdapterConnection connection(*serial);
    connection.select_port("ttyUSB0");
}

TEST(TestAdapterConnection, openAndIsOpenAskTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("ttyUSB0")));
    EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(Return(false));
    AdapterConnection connection(*serial);
    ASSERT_EQ(connection.open(), QString("ttyUSB0"));
    ASSERT_TRUE(!connection.is_open());
}

TEST(TestAdapterConnection, openedPortAsksTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), get_openedSerialPort()).WillOnce(Return(QString("ttyUSB0")));
    AdapterConnection connection(*serial);
    ASSERT_EQ(connection.opened_port(), QString("ttyUSB0"));
}

TEST(TestAdapterConnection, canTransportIsRawCanElevenBit)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_can_connection(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_can_speed(QString("500000"))).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), reset_connection());
    }
    AdapterConnection connection(*serial);
    connection.apply_log_transport(LogTransport::Can, false);
}

// Pinned: log_transport_changed set 29-bit identifiers for iso15765.
TEST(TestAdapterConnection, iso15765TransportIsTwentyNineBit)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_29_bit_id(true)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_can_speed(QString("500000"))).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), reset_connection());
    }
    AdapterConnection connection(*serial);
    connection.apply_log_transport(LogTransport::Iso15765, false);
}

TEST(TestAdapterConnection, klineWithSsmRunsAtFourThousandEightHundred)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), change_port_speed(QString("4800"))).WillOnce(Return(0));
        EXPECT_CALL(serial.fake(), reset_connection());
    }
    AdapterConnection connection(*serial);
    connection.apply_log_transport(LogTransport::KLine, true);
}

TEST(TestAdapterConnection, klineWithoutSsmLeavesTheSpeedAlone)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), change_port_speed(_)).Times(0);
    EXPECT_CALL(serial.fake(), reset_connection());
    AdapterConnection connection(*serial);
    connection.apply_log_transport(LogTransport::KLine, false);
}

TEST(TestAdapterConnection, clearLinkFlagsClearsEveryFlagAndKeepsParity)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), reset_connection());
        EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("4800"))).WillOnce(Return(true));
    }
    EXPECT_CALL(serial.fake(), set_serial_port_parity(_)).Times(0);
    AdapterConnection connection(*serial);
    connection.clear_link_flags();
}

TEST(TestAdapterConnection, returnToIdleResetsBaudAndParityAndKeepsTheFlags)
{
    FakeBackedSerial serial;
    {
        InSequence order;
        EXPECT_CALL(serial.fake(), reset_connection());
        EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("4800"))).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)))
            .WillOnce(Return(true));
    }
    EXPECT_CALL(serial.fake(), set_is_iso14230_connection(_)).Times(0);
    EXPECT_CALL(serial.fake(), set_is_29_bit_id(_)).Times(0);
    EXPECT_CALL(serial.fake(), set_add_iso14230_header(_)).Times(0);
    EXPECT_CALL(serial.fake(), set_is_can_connection(_)).Times(0);
    EXPECT_CALL(serial.fake(), set_is_iso15765_connection(_)).Times(0);
    AdapterConnection connection(*serial);
    connection.return_to_idle();
}

TEST(TestAdapterConnection, setPortSpeedPassesTheBaudAsText)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), change_port_speed(QString("4800"))).WillOnce(Return(0));
    AdapterConnection connection(*serial);
    connection.set_port_speed(4800);
}

TEST(TestAdapterConnection, batteryIsReadOnlyFromAnOpenPort)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillOnce(Return(false)).WillOnce(Return(true));
    EXPECT_CALL(serial.fake(), read_vbatt()).WillOnce(Return(12500UL));
    AdapterConnection connection(*serial);
    ASSERT_TRUE(!connection.battery_millivolts().has_value());
    const std::optional<unsigned long> reading = connection.battery_millivolts();
    ASSERT_TRUE(reading.has_value());
    ASSERT_EQ(*reading, 12500UL);
}

TEST(TestAdapterConnection, waitForSourceWaitsOnTheFacade)
{
    FakeBackedSerial serial;
    EXPECT_CALL(serial.fake(), waitForSource());
    AdapterConnection connection(*serial);
    connection.wait_for_source();
}

TEST(TestAdapterConnection, forwardsFacadeStateChanges)
{
    FakeBackedSerial serial;
    AdapterConnection connection(*serial);
    fastecu::testing::SignalRecorder spy(&connection, &AdapterConnection::stateChanged);
    emit serial->stateChanged(QRemoteObjectReplica::Valid, QRemoteObjectReplica::Default);
    ASSERT_EQ(spy.count(), 1U);
}

TEST(TestAdapterConnection, exposesTheSameFacade)
{
    FakeBackedSerial serial;
    AdapterConnection connection(*serial);
    ASSERT_EQ(&connection.facade(), serial.get());
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
