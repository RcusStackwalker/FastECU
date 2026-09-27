#include "src/backend/ports/testing/mock_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::MockClock;
using ::testing::_;
using ::testing::DoAll;
using ::testing::InvokeWithoutArgs;
using ::testing::Return;

TEST(MockClock, DefaultSleepAdvancesElapsedTime)
{
    MockClock clock;
    FakeCancellationToken active;

    ASSERT_THAT(clock.sleep(5ms, active), fastecu::testing::IsOk());
    ASSERT_THAT(clock.sleep(20ms, active), fastecu::testing::IsOk());

    EXPECT_EQ(clock.elapsed(), 25ms);
}

TEST(MockClock, DefaultNowTracksElapsedTime)
{
    MockClock clock;
    FakeCancellationToken active;
    const auto before = clock.now();

    ASSERT_THAT(clock.sleep(7ms, active), fastecu::testing::IsOk());

    EXPECT_EQ(clock.now() - before, 7ms);
}

TEST(MockClock, DefaultSleepHonoursCancellation)
{
    MockClock clock;
    FakeCancellationToken cancelled{true};

    EXPECT_THAT(clock.sleep(5ms, cancelled), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(clock.elapsed(), 0ms);
}

TEST(MockClock, UnmentionedSleepsStayAllowedAlongsideASpecificExpectation)
{
    MockClock clock;
    FakeCancellationToken active;
    EXPECT_CALL(clock, sleep(3ms, _)).Times(1);

    ASSERT_THAT(clock.sleep(200ms, active), fastecu::testing::IsOk());
    ASSERT_THAT(clock.sleep(3ms, active), fastecu::testing::IsOk());
    ASSERT_THAT(clock.sleep(1ms, active), fastecu::testing::IsOk());

    EXPECT_EQ(clock.elapsed(), 204ms);
}

TEST(MockClock, ASpecificExpectationOverridesTheDefaultAction)
{
    MockClock clock;
    FakeCancellationToken active;
    EXPECT_CALL(clock, sleep(5000ms, _)).WillOnce(Return(fastecu::fail(ErrorKind::Cancelled, "upload delay")));

    EXPECT_THAT(clock.sleep(5000ms, active), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(clock.elapsed(), 0ms);
}

TEST(MockClock, SleepOnFakeComposesWithASideEffect)
{
    MockClock clock;
    FakeCancellationToken cancellation;
    EXPECT_CALL(clock, sleep(3ms, _))
        .WillOnce(DoAll(InvokeWithoutArgs([&] { cancellation.set_cancelled(true); }), clock.sleep_on_fake()));

    EXPECT_THAT(clock.sleep(3ms, cancellation), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_TRUE(cancellation.cancelled());
}

TEST(MockClock, AnActionCanSleepOnTheFakeThenActAndReturnTheSleepResult)
{
    MockClock clock;
    FakeCancellationToken cancellation;
    int sleeps_seen = 0;
    ON_CALL(clock, sleep)
        .WillByDefault(
            [&](std::chrono::milliseconds duration, const fastecu::ICancellationToken& token)
            {
                const fastecu::Status result = clock.fake().sleep(duration, token);
                if (++sleeps_seen == 1)
                {
                    cancellation.set_cancelled(true);
                }
                return result;
            });

    EXPECT_THAT(clock.sleep(4ms, cancellation), fastecu::testing::IsOk());
    EXPECT_THAT(clock.sleep(4ms, cancellation), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(clock.elapsed(), 4ms);
}

TEST(MockClock, TimesZeroCatchAllForbidsOtherSleeps)
{
    EXPECT_NONFATAL_FAILURE(
        {
            MockClock clock;
            FakeCancellationToken active;
            EXPECT_CALL(clock, sleep).Times(0);
            EXPECT_CALL(clock, sleep(500ms, _)).Times(1);
            (void)clock.sleep(500ms, active);
            (void)clock.sleep(1ms, active);
        },
        "called more times than expected");
}
