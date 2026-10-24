#include "src/ui/desktop/calibration/session_key.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::Eq;

namespace fastecu::ui
{
namespace
{

using calibration::SessionId;

TEST(SessionKey, RoundTripsDecimalText)
{
    EXPECT_EQ(sessionKeyText(SessionId{42}), QString("42"));
    EXPECT_EQ(parseSessionKey("42"), SessionId{42});
    EXPECT_EQ(parseSessionKey(sessionKeyText(SessionId{18446744073709551615ULL})), SessionId{18446744073709551615ULL});
}

TEST(SessionKey, RejectsAnythingButPlainDecimal)
{
    EXPECT_THAT(parseSessionKey(""), Eq(std::nullopt));
    EXPECT_THAT(parseSessionKey("-1"), Eq(std::nullopt));
    EXPECT_THAT(parseSessionKey("+1"), Eq(std::nullopt));
    EXPECT_THAT(parseSessionKey("1a"), Eq(std::nullopt));
    EXPECT_THAT(parseSessionKey(" 1"), Eq(std::nullopt));
    EXPECT_THAT(parseSessionKey("18446744073709551616"), Eq(std::nullopt)); // overflow
}

} // namespace
} // namespace fastecu::ui
