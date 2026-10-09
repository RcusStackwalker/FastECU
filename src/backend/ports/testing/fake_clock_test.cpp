#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include <gtest/gtest.h>

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;

TEST(FakeClock, OptionalAutoAdvancePreservesSsmTimingModel)
{
    FakeClock clock;
    clock.SetNowAutoAdvance(10ms);
    clock.SetSleepAdvance(10ms);
    fastecu::FakeCancellationToken active;

    EXPECT_EQ(clock.Elapsed(), 0ms);
    const auto first = clock.Now();
    const auto second = clock.Now();
    EXPECT_EQ(second - first, 10ms);
    EXPECT_EQ(clock.Elapsed(), 20ms);
    // set_sleep_advance overrides the requested duration, so 999ms advances by 10ms.
    ASSERT_TRUE(clock.Sleep(999ms, active));
    EXPECT_EQ(clock.Elapsed(), 30ms);
}

TEST(FakeClock, MakeAutoAdvancingClockConfiguresBothTimingModels)
{
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken active;

    const auto first = clock.Now();
    const auto second = clock.Now();
    EXPECT_EQ(second - first, 10ms);
    ASSERT_TRUE(clock.Sleep(999ms, active));
    EXPECT_EQ(clock.Elapsed(), 30ms);
}

TEST(Clock, SleepAdvancesAndSucceeds)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    EXPECT_THAT(c.Sleep(10ms, t), fastecu::testing::IsOk());
    EXPECT_EQ(c.Elapsed(), 10ms);
}

TEST(Clock, SleepReturnsCancelledWhenTokenSet)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    t.SetCancelled(true);
    ASSERT_THAT(c.Sleep(10ms, t), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(c.Elapsed(), 0ms);
}

TEST(Clock, NegativeSleepDoesNotRewindTheClock)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    ASSERT_TRUE(c.Sleep(-5ms, t));
    EXPECT_EQ(c.Elapsed(), 0ms);
}
