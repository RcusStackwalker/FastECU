#include "src/backend/ports/manual_cancellation_token.h"

#include <gtest/gtest.h>

TEST(ManualCancellationToken, StartsNotCancelled)
{
    fastecu::ManualCancellationToken token;
    EXPECT_FALSE(token.Cancelled());
}

TEST(ManualCancellationToken, CancelSetsFlag)
{
    fastecu::ManualCancellationToken token;
    token.Cancel();
    EXPECT_TRUE(token.Cancelled());
}

TEST(ManualCancellationToken, CancelIsIdempotent)
{
    fastecu::ManualCancellationToken token;
    token.Cancel();
    token.Cancel();
    EXPECT_TRUE(token.Cancelled());
}
