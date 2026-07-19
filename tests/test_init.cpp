#include <gtest/gtest.h>

#include "src/backend/protocol/imut_dma_init.h"
#include "scripted_kline_transport.h"

using namespace mutdma;

TEST(TestInit, already_in_mode_just_sets_baud)
{
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    ASSERT_TRUE(init.wake(t));
    ASSERT_TRUE(t.scriptConsumed()); // no writes expected
}
