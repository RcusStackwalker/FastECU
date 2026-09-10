#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include <gtest/gtest.h>

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::Status;

TEST(FakeClock, OptionalAutoAdvancePreservesSsmTimingModel)
{
    FakeClock clock;
    clock.set_now_auto_advance(10ms);
    clock.set_sleep_advance(10ms);
    fastecu::FakeCancellationToken active;

    EXPECT_EQ(clock.elapsed(), 0ms);
    const auto first = clock.now();
    const auto second = clock.now();
    EXPECT_EQ(second - first, 10ms);
    EXPECT_EQ(clock.elapsed(), 20ms);
    // set_sleep_advance overrides the requested duration, so 999ms advances by 10ms.
    ASSERT_TRUE(clock.sleep(999ms, active));
    EXPECT_EQ(clock.elapsed(), 30ms);
}

TEST(FakeClock, MakeAutoAdvancingClockConfiguresBothTimingModels)
{
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken active;

    const auto first = clock.now();
    const auto second = clock.now();
    EXPECT_EQ(second - first, 10ms);
    ASSERT_TRUE(clock.sleep(999ms, active));
    EXPECT_EQ(clock.elapsed(), 30ms);
}

TEST(Clock, SleepAdvancesAndSucceeds)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    Status s = c.sleep(10ms, t);
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(c.elapsed(), 10ms);
}

TEST(Clock, SleepReturnsCancelledWhenTokenSet)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    t.set_cancelled(true);
    Status s = c.sleep(10ms, t);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(c.elapsed(), 0ms);
}

TEST(Clock, NegativeSleepDoesNotRewindTheClock)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    ASSERT_TRUE(c.sleep(-5ms, t));
    EXPECT_EQ(c.elapsed(), 0ms);
}
