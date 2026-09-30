#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/serial/serial_idle.h"

#include <QCoreApplication>
#include <QSerialPort>
#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <cstdint>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

TEST(SerialIdleTest, resetsTheConnectionThenRestoresTheIdleLineSettingsInOrder)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial{[&fake]() -> SerialBackend *
                             {
                                 fake = new NiceFakeBackend;
                                 return fake;
                             }};
    ASSERT_TRUE(serial.set_add_ssm_header(false)); // forces the backend into existence
    ASSERT_TRUE(fake != nullptr);

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

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
