#include "src/backend/ports/testing/fake_cancellation_token.h"

#include <gtest/gtest.h>

TEST(FakeCancellationToken, SupportsFixedMutableAndCheckCountBehavior)
{
    fastecu::FakeCancellationToken token;
    EXPECT_FALSE(token.cancelled());
    token.set_cancelled(true);
    EXPECT_TRUE(token.cancelled());

    fastecu::FakeCancellationToken counted;
    counted.cancel_on_check(3);
    EXPECT_FALSE(counted.cancelled());
    EXPECT_FALSE(counted.cancelled());
    EXPECT_TRUE(counted.cancelled());
    EXPECT_EQ(counted.check_count(), 3u);
}

TEST(FakeCancellationToken, PredicateCanObserveAnotherDouble)
{
    int polls = 0;
    fastecu::FakeCancellationToken token;
    token.set_predicate([&polls]
                        { return polls >= 2; });
    EXPECT_FALSE(token.cancelled());
    polls = 2;
    EXPECT_TRUE(token.cancelled());
}
