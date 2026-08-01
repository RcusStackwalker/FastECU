#include "src/backend/definition/text_format.h"

#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{

TEST(ParseHexValueTest, AcceptsPrefixedAndBareHex)
{
    EXPECT_EQ(parse_hex_value("0xFFFF6004"), 0xFFFF6004u);
    EXPECT_EQ(parse_hex_value("0XFFFF3000"), 0xFFFF3000u);
    EXPECT_EQ(parse_hex_value("ffff4000"), 0xFFFF4000u);
    EXPECT_EQ(parse_hex_value("0"), 0u);
}

TEST(ParseHexValueTest, TrimsSurroundingWhitespace)
{
    EXPECT_EQ(parse_hex_value("  0xFFFF6004  "), 0xFFFF6004u);
}

TEST(ParseHexValueTest, RejectsMalformedInput)
{
    EXPECT_FALSE(parse_hex_value("").has_value());
    EXPECT_FALSE(parse_hex_value("0x").has_value());
    EXPECT_FALSE(parse_hex_value("nonsense").has_value());
    EXPECT_FALSE(parse_hex_value("0xFFFF6004xyz").has_value()); // trailing junk
    EXPECT_FALSE(parse_hex_value("-1").has_value());
}

TEST(ParseHexValueTest, RoundTripsWithHexText)
{
    EXPECT_EQ(parse_hex_value(hex_text(0xDEADBEEFu)), 0xDEADBEEFu);
}

} // namespace
} // namespace fastecu::definition
