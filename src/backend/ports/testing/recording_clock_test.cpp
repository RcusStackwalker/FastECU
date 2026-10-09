#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::RecordingClock;

TEST(RecordingClock, RecordsEachRequestedDurationInOrderAndAdvances)
{
    RecordingClock clock;
    FakeCancellationToken active;

    ASSERT_TRUE(clock.sleep(5ms, active));
    ASSERT_TRUE(clock.sleep(20ms, active));

    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{5ms, 20ms}));
    EXPECT_EQ(clock.elapsed(), 25ms);
}

TEST(RecordingClock, RecordsACancelledSleepWithoutAdvancing)
{
    RecordingClock clock;
    FakeCancellationToken cancelled{true};

    EXPECT_THAT(clock.sleep(7ms, cancelled), fastecu::testing::IsErr(ErrorKind::kCancelled));

    EXPECT_EQ(clock.sleep_calls, (std::vector<std::chrono::milliseconds>{7ms}));
    EXPECT_EQ(clock.elapsed(), 0ms);
}
