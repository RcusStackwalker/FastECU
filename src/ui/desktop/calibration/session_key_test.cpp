#include "src/ui/desktop/calibration/session_key.h"

#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{

using calibration::SessionId;

TEST(SessionKey, RoundTripsDecimalText)
{
    EXPECT_EQ(session_key_text(SessionId{42}), QString("42"));
    EXPECT_EQ(parse_session_key("42"), SessionId{42});
    EXPECT_EQ(parse_session_key(session_key_text(SessionId{18446744073709551615ULL})),
              SessionId{18446744073709551615ULL});
}

TEST(SessionKey, RejectsAnythingButPlainDecimal)
{
    EXPECT_FALSE(parse_session_key("").has_value());
    EXPECT_FALSE(parse_session_key("-1").has_value());
    EXPECT_FALSE(parse_session_key("+1").has_value());
    EXPECT_FALSE(parse_session_key("1a").has_value());
    EXPECT_FALSE(parse_session_key(" 1").has_value());
    EXPECT_FALSE(parse_session_key("18446744073709551616").has_value()); // overflow
}

} // namespace
} // namespace fastecu::ui
