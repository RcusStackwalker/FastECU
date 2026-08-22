#include "src/backend/ports/manual_cancellation_token.h"

#include <gtest/gtest.h>

TEST(ManualCancellationToken, StartsNotCancelled)
{
    fastecu::ManualCancellationToken token;
    EXPECT_FALSE(token.cancelled());
}

TEST(ManualCancellationToken, CancelSetsFlag)
{
    fastecu::ManualCancellationToken token;
    token.cancel();
    EXPECT_TRUE(token.cancelled());
}

TEST(ManualCancellationToken, CancelIsIdempotent)
{
    fastecu::ManualCancellationToken token;
    token.cancel();
    token.cancel();
    EXPECT_TRUE(token.cancelled());
}
