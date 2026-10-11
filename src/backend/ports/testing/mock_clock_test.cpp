#include "src/backend/ports/testing/mock_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <chrono>
#include <tuple>

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::MockClock;
using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;

TEST(MockClock, DefaultSleepAdvancesElapsedTime)
{
    MockClock clock;
    FakeCancellationToken active;

    ASSERT_THAT(clock.Sleep(5ms, active), fastecu::testing::IsOk());
    ASSERT_THAT(clock.Sleep(20ms, active), fastecu::testing::IsOk());

    EXPECT_EQ(clock.Elapsed(), 25ms);
}

TEST(MockClock, DefaultNowTracksElapsedTime)
{
    MockClock clock;
    FakeCancellationToken active;
    const auto before = clock.Now();

    ASSERT_THAT(clock.Sleep(7ms, active), fastecu::testing::IsOk());

    EXPECT_EQ(clock.Now() - before, 7ms);
}

TEST(MockClock, DefaultSleepHonoursCancellation)
{
    MockClock clock;
    FakeCancellationToken cancelled{true};

    EXPECT_THAT(clock.Sleep(5ms, cancelled), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(clock.Elapsed(), 0ms);
}

TEST(MockClock, UnmentionedSleepsStayAllowedAlongsideASpecificExpectation)
{
    MockClock clock;
    FakeCancellationToken active;
    EXPECT_CALL(clock, Sleep(3ms, _)).Times(1);

    ASSERT_THAT(clock.Sleep(200ms, active), fastecu::testing::IsOk());
    ASSERT_THAT(clock.Sleep(3ms, active), fastecu::testing::IsOk());
    ASSERT_THAT(clock.Sleep(1ms, active), fastecu::testing::IsOk());

    EXPECT_EQ(clock.Elapsed(), 204ms);
}

TEST(MockClock, ASpecificExpectationOverridesTheDefaultAction)
{
    MockClock clock;
    FakeCancellationToken active;
    EXPECT_CALL(clock, Sleep(5000ms, _)).WillOnce(Return(fastecu::Fail(ErrorKind::kCancelled, "upload delay")));

    EXPECT_THAT(clock.Sleep(5000ms, active), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(clock.Elapsed(), 0ms);
}

TEST(MockClock, SleepOnFakeComposesWithASideEffect)
{
    MockClock clock;
    FakeCancellationToken cancellation;
    EXPECT_CALL(clock, Sleep(3ms, _)).WillOnce(DoAll([&] { cancellation.SetCancelled(true); }, clock.SleepOnFake()));

    EXPECT_THAT(clock.Sleep(3ms, cancellation), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(cancellation.Cancelled());
}

TEST(MockClock, AnActionCanSleepOnTheFakeThenActAndReturnTheSleepResult)
{
    MockClock clock;
    FakeCancellationToken cancellation;
    int sleeps_seen = 0;
    ON_CALL(clock, Sleep)
        .WillByDefault(
            [&](std::chrono::milliseconds duration, const fastecu::ICancellationToken& token)
            {
                const fastecu::Status result = clock.Fake().Sleep(duration, token);
                if (++sleeps_seen == 1)
                {
                    cancellation.SetCancelled(true);
                }
                return result;
            });

    EXPECT_THAT(clock.Sleep(4ms, cancellation), fastecu::testing::IsOk());
    EXPECT_THAT(clock.Sleep(4ms, cancellation), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(clock.Elapsed(), 4ms);
}

TEST(MockClock, TimesZeroCatchAllForbidsOtherSleeps)
{
    EXPECT_NONFATAL_FAILURE(
        {
            MockClock clock;
            FakeCancellationToken active;
            EXPECT_CALL(clock, Sleep).Times(0);
            EXPECT_CALL(clock, Sleep(500ms, _)).Times(1);
            std::ignore = clock.Sleep(500ms, active);
            std::ignore = clock.Sleep(1ms, active);
        },
        "called more times than expected");
}
