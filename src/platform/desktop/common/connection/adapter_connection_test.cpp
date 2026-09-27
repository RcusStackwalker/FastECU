#include "src/platform/desktop/common/connection/adapter_connection.h"

#include <QCoreApplication>
#include <QSerialPort>
#include <QSignalSpy>
#include <QTest>

#include <cstdint>
#include <optional>

#include <gmock/gmock.h>

#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/transport/fake_backed_serial.h"

using fastecu::desktop::connection::AdapterConnection;
using fastecu::desktop::connection::log_transport_from_text;
using fastecu::desktop::connection::LogTransport;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;

class TestAdapterConnection : public QObject
{
    Q_OBJECT

  private slots:

    void parsesTheToolbarTransportText()
    {
        QCOMPARE(log_transport_from_text("CAN"), LogTransport::Can);
        QCOMPARE(log_transport_from_text("iso15765"), LogTransport::Iso15765);
        QCOMPARE(log_transport_from_text("K-Line"), LogTransport::KLine);
        QCOMPARE(log_transport_from_text("SSM"), LogTransport::Ssm);
        QCOMPARE(log_transport_from_text(""), LogTransport::Other);
    }

    void listsPortsFromTheFacade()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), check_serial_ports()).WillOnce(Return(QStringList{"ttyUSB0 - FT232"}));
        AdapterConnection connection(*serial);
        QCOMPARE(connection.available_ports(), QStringList{"ttyUSB0 - FT232"});
    }

    void setInitialPortSetsTheBaudThenThePort()
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

    void selectPortReplacesThePortList()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), set_serial_port_list(QStringList{"ttyUSB0"})).WillOnce(Return(true));
        AdapterConnection connection(*serial);
        connection.select_port("ttyUSB0");
    }

    void openAndIsOpenAskTheFacade()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("ttyUSB0")));
        EXPECT_CALL(serial.fake(), is_serial_port_open()).WillOnce(Return(false));
        AdapterConnection connection(*serial);
        QCOMPARE(connection.open(), QString("ttyUSB0"));
        QVERIFY(!connection.is_open());
    }

    void canTransportIsRawCanElevenBit()
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
    void iso15765TransportIsTwentyNineBit()
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

    void klineWithSsmRunsAtFourThousandEightHundred()
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

    void klineWithoutSsmLeavesTheSpeedAlone()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), change_port_speed(_)).Times(0);
        EXPECT_CALL(serial.fake(), reset_connection());
        AdapterConnection connection(*serial);
        connection.apply_log_transport(LogTransport::KLine, false);
    }

    void clearLinkFlagsClearsEveryFlagAndKeepsParity()
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

    void returnToIdleResetsBaudAndParityAndKeepsTheFlags()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), reset_connection());
            EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("4800"))).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)))
                .WillOnce(Return(true));
        }
        EXPECT_CALL(serial.fake(), set_is_can_connection(_)).Times(0);
        EXPECT_CALL(serial.fake(), set_is_iso15765_connection(_)).Times(0);
        AdapterConnection connection(*serial);
        connection.return_to_idle();
    }

    void setPortSpeedPassesTheBaudAsText()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), change_port_speed(QString("4800"))).WillOnce(Return(0));
        AdapterConnection connection(*serial);
        connection.set_port_speed(4800);
    }

    void batteryIsReadOnlyFromAnOpenPort()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillOnce(Return(false)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), read_vbatt()).WillOnce(Return(12500UL));
        AdapterConnection connection(*serial);
        QVERIFY(!connection.battery_millivolts().has_value());
        const std::optional<unsigned long> reading = connection.battery_millivolts();
        QVERIFY(reading.has_value());
        QCOMPARE(*reading, 12500UL);
    }

    void waitForSourceWaitsOnTheFacade()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), waitForSource());
        AdapterConnection connection(*serial);
        connection.wait_for_source();
    }

    void forwardsFacadeStateChanges()
    {
        FakeBackedSerial serial;
        AdapterConnection connection(*serial);
        QSignalSpy spy(&connection, &AdapterConnection::stateChanged);
        emit serial->stateChanged(QRemoteObjectReplica::Valid, QRemoteObjectReplica::Default);
        QCOMPARE(spy.count(), 1);
    }

    void exposesTheSameFacade()
    {
        FakeBackedSerial serial;
        AdapterConnection connection(*serial);
        QCOMPARE(&connection.facade(), serial.get());
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    TestAdapterConnection test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "adapter_connection_test.moc"
