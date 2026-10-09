#include "src/ui/desktop/calibration/session_key.h"

#include <gtest/gtest.h>

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
    EXPECT_FALSE(parseSessionKey("").has_value());
    EXPECT_FALSE(parseSessionKey("-1").has_value());
    EXPECT_FALSE(parseSessionKey("+1").has_value());
    EXPECT_FALSE(parseSessionKey("1a").has_value());
    EXPECT_FALSE(parseSessionKey(" 1").has_value());
    EXPECT_FALSE(parseSessionKey("18446744073709551616").has_value()); // overflow
}

} // namespace
} // namespace fastecu::ui
