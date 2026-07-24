#include "src/backend/flash/flash_cancellation.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

TEST(CancellationSourceTest, StartsNotCancelled)
{
    CancellationSource source;
    EXPECT_FALSE(source.token().cancelled());
}

TEST(CancellationSourceTest, TripMakesTokenReportCancelled)
{
    CancellationSource source;
    source.trip();
    EXPECT_TRUE(source.token().cancelled());
}

TEST(CancellationSourceTest, TripIsIdempotent)
{
    CancellationSource source;
    source.trip();
    source.trip();
    EXPECT_TRUE(source.token().cancelled());
}

} // namespace
} // namespace fastecu::flash
