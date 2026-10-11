#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/serial/serial_idle.h"

#include <QCoreApplication>
#include <QSerialPort>
#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <cstdint>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

TEST(SerialIdleTest, resetsTheConnectionThenRestoresTheIdleLineSettingsInOrder)
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial{[&fake]() -> SerialBackend *
                             {
                                 fake = new NiceFakeBackend;
                                 return fake;
                             }};
    ASSERT_TRUE(serial.SetAddSsmHeader(false)); // forces the backend into existence
    ASSERT_TRUE(fake != nullptr);

    {
        ::testing::InSequence sequence;
        EXPECT_CALL(*fake, ResetConnection());
        EXPECT_CALL(*fake, SetIsIso14230Connection(false));
        EXPECT_CALL(*fake, SetIs29BitId(false));
        EXPECT_CALL(*fake, SetAddIso14230Header(false));
        EXPECT_CALL(*fake, SetIsCanConnection(false));
        EXPECT_CALL(*fake, SetIsIso15765Connection(false));
        EXPECT_CALL(*fake, SetSerialPortParity(static_cast<std::uint8_t>(QSerialPort::NoParity)));
        EXPECT_CALL(*fake, SetSerialPortBaudrate(QStringLiteral("4800")));
    }

    fastecu::desktop::serial::ResetSerialToIdle(serial);
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
