#include "src/platform/desktop/common/serial/serial_idle.h"

#include <QCoreApplication>
#include <QSerialPort>
#include <QTest>

#include <gmock/gmock.h>

#include <cstdint>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

class SerialIdleTest : public QObject
{
    Q_OBJECT

  private slots:
    void resetsTheConnectionThenRestoresTheIdleLineSettingsInOrder()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial{"", "", nullptr, nullptr, [&fake]() -> SerialBackend *
                                 {
                                     fake = new NiceFakeBackend;
                                     return fake;
                                 }};
        QVERIFY(serial.set_add_ssm_header(false)); // forces the backend into existence
        QVERIFY(fake != nullptr);

        {
            ::testing::InSequence sequence;
            EXPECT_CALL(*fake, reset_connection());
            EXPECT_CALL(*fake, set_is_iso14230_connection(false));
            EXPECT_CALL(*fake, set_is_29_bit_id(false));
            EXPECT_CALL(*fake, set_add_iso14230_header(false));
            EXPECT_CALL(*fake, set_is_can_connection(false));
            EXPECT_CALL(*fake, set_is_iso15765_connection(false));
            EXPECT_CALL(*fake, set_serial_port_parity(static_cast<std::uint8_t>(QSerialPort::NoParity)));
            EXPECT_CALL(*fake, set_serial_port_baudrate(QStringLiteral("4800")));
        }

        fastecu::desktop::serial::reset_serial_to_idle(serial);
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    SerialIdleTest test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "serial_idle_test.moc"
