#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

TEST(ScriptedCanFlashTransportTest, DefaultsClosed)
{
    ScriptedCanFlashTransport transport;

    EXPECT_FALSE(transport.is_open());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(ScriptedCanFlashTransportTest, ExplicitOpenStateStartsOpenWithoutLifecycleCalls)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};

    EXPECT_TRUE(transport.is_open());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(ScriptedKlineFlashTransportTest, DefaultsClosed)
{
    ScriptedKlineFlashTransport transport;

    EXPECT_FALSE(transport.isOpen());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(ScriptedKlineFlashTransportTest, ExplicitOpenStateStartsOpenWithoutLifecycleCalls)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};

    EXPECT_TRUE(transport.isOpen());
    EXPECT_EQ(transport.close_call_count_, 0);
}

} // namespace
} // namespace fastecu::flash
