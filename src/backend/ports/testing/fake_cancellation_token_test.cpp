#include "src/backend/ports/testing/fake_cancellation_token.h"

#include <gtest/gtest.h>

#include <thread>
#include <tuple>
#include <vector>

TEST(FakeCancellationToken, SupportsFixedMutableAndCheckCountBehavior)
{
    fastecu::FakeCancellationToken token;
    EXPECT_FALSE(token.Cancelled());
    token.SetCancelled(true);
    EXPECT_TRUE(token.Cancelled());

    fastecu::FakeCancellationToken counted;
    counted.CancelOnCheck(3);
    EXPECT_FALSE(counted.Cancelled());
    EXPECT_FALSE(counted.Cancelled());
    EXPECT_TRUE(counted.Cancelled());
    EXPECT_EQ(counted.CheckCount(), 3U);
}

TEST(FakeCancellationToken, PredicateCanObserveAnotherDouble)
{
    int polls = 0;
    fastecu::FakeCancellationToken token;
    token.SetPredicate([&polls] { return polls >= 2; });
    EXPECT_FALSE(token.Cancelled());
    polls = 2;
    EXPECT_TRUE(token.Cancelled());
}

TEST(FakeCancellationToken, PredicateThenCheckThresholdThenFixedStateTakePrecedence)
{
    fastecu::FakeCancellationToken all_modes(true);
    all_modes.CancelOnCheck(1);
    all_modes.SetPredicate([] { return false; });

    EXPECT_FALSE(all_modes.Cancelled());

    all_modes.SetPredicate({});
    EXPECT_TRUE(all_modes.Cancelled());

    fastecu::FakeCancellationToken threshold_over_fixed(true);
    threshold_over_fixed.CancelOnCheck(2);
    EXPECT_FALSE(threshold_over_fixed.Cancelled());
    EXPECT_TRUE(threshold_over_fixed.Cancelled());

    fastecu::FakeCancellationToken fixed_state(true);
    EXPECT_TRUE(fixed_state.Cancelled());
}

TEST(FakeCancellationToken, ConcurrentChecksAreCountedExactly)
{
    constexpr std::size_t kThreadCount = 8;
    constexpr std::size_t kChecksPerThread = 100'000;
    fastecu::FakeCancellationToken token;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (std::size_t i = 0; i < kThreadCount; ++i)
    {
        threads.emplace_back(
            [&token]
            {
                for (std::size_t check = 0; check < kChecksPerThread; ++check)
                {
                    std::ignore = token.Cancelled();
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(token.CheckCount(), kThreadCount * kChecksPerThread);
}
