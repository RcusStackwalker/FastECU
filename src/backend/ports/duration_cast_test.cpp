#include "src/backend/ports/duration_cast.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace std::chrono_literals;
using fastecu::saturating_ms;

TEST(SaturatingMs, ConvertsAValueInRange)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(500ms), 500);
    EXPECT_EQ(saturating_ms<int>(3000ms), 3000);
    EXPECT_EQ(saturating_ms<unsigned long>(200ms), 200UL);
}

TEST(SaturatingMs, ZeroStaysZero)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(0ms), 0);
}

TEST(SaturatingMs, NegativeClampsToZero)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(-1ms), 0);
    EXPECT_EQ(saturating_ms<int>(-5000ms), 0);
}

TEST(SaturatingMs, ExactMaximumIsPreserved)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(std::chrono::milliseconds{65535}), 65535);
}

TEST(SaturatingMs, AboveMaximumSaturatesRatherThanWrapping)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(std::chrono::milliseconds{65536}), 65535);
    EXPECT_EQ(saturating_ms<std::uint16_t>(70000ms), 65535);
}

TEST(SaturatingMs, DurationMaxSaturates)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(std::chrono::milliseconds::max()), 65535);
    EXPECT_EQ(saturating_ms<int>(std::chrono::milliseconds::max()), std::numeric_limits<int>::max());
}

TEST(SaturatingMs, IsUsableInAConstantExpression)
{
    static_assert(saturating_ms<std::uint16_t>(70000ms) == 65535);
    SUCCEED();
}
