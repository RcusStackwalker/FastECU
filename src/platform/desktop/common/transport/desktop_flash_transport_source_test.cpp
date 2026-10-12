#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/transport/desktop_flash_transport_source.h"

#include <gtest/gtest.h>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"

using fastecu::flash::DesktopCanFlashTransport;
using fastecu::flash::DesktopFlashTransportSource;
using fastecu::flash::DesktopKlineFlashTransport;
using fastecu::flash::DesktopMixedCanFlashTransport;

// Moved from TestDesktopKlineFlashTransport: the source now answers the
// question, and it reads the adapter's current setting on every call.
TEST(TestDesktopFlashTransportSource, programmingVoltageSupplyMirrorsOpenPort2OnEveryPlatform)
{
    EXPECT_FALSE(DesktopFlashTransportSource(nullptr).SuppliesProgrammingVoltage());

    FakeBackedSerial serial;
    SerialPortActions *serial_ptr = serial.Get();
    const DesktopFlashTransportSource source(serial_ptr);
    EXPECT_FALSE(source.SuppliesProgrammingVoltage());
    ASSERT_TRUE(serial_ptr->SetUseOpenport2Adapter(true));
    EXPECT_TRUE(source.SuppliesProgrammingVoltage());
}

TEST(TestDesktopFlashTransportSource, makesTheDesktopTransportOfEachKind)
{
    FakeBackedSerial serial;
    for (SerialPortActions *adapter : {serial.Get(), static_cast<SerialPortActions *>(nullptr)})
    {
        DesktopFlashTransportSource source(adapter);
        const auto kline = source.MakeKline();
        const auto can = source.MakeCan();
        const auto mixed_can = source.MakeMixedCan();
        EXPECT_NE(dynamic_cast<DesktopKlineFlashTransport *>(kline.get()), nullptr);
        EXPECT_NE(dynamic_cast<DesktopCanFlashTransport *>(can.get()), nullptr);
        EXPECT_NE(dynamic_cast<DesktopMixedCanFlashTransport *>(mixed_can.get()), nullptr);
    }
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
