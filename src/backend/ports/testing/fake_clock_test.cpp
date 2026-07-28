#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include <gtest/gtest.h>

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::Status;

TEST(FakeClock, OptionalAutoAdvancePreservesSsmTimingModel)
{
    FakeClock clock;
    clock.set_now_auto_advance_ms(10);
    clock.set_sleep_advance_ms(10);
    fastecu::FakeCancellationToken active;

    EXPECT_EQ(clock.now_ms(), 0u);
    EXPECT_EQ(clock.now_ms(), 10u);
    ASSERT_TRUE(clock.sleep(999, active));
    EXPECT_EQ(clock.now_ms(), 30u);
}

TEST(FakeClock, MakeAutoAdvancingClockConfiguresBothTimingModels)
{
    auto clock = fastecu::make_auto_advancing_clock(10);
    fastecu::FakeCancellationToken active;

    EXPECT_EQ(clock.now_ms(), 0u);
    EXPECT_EQ(clock.now_ms(), 10u);
    ASSERT_TRUE(clock.sleep(999, active));
    EXPECT_EQ(clock.now_ms(), 30u);
}

TEST(Clock, SleepAdvancesAndSucceeds)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    Status s = c.sleep(10, t);
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(c.now_ms(), 10u);
}

TEST(Clock, SleepReturnsCancelledWhenTokenSet)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    t.set_cancelled(true);
    Status s = c.sleep(10, t);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(c.now_ms(), 0u);
}
