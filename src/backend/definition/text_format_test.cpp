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

TEST(ParseHexValueTest, TrimsVerticalTabAndFormFeed)
{
    // std::isspace (the original trim_copy's basis) matches '\v' and '\f' in
    // the C locale in addition to ' ', '\t', '\r', '\n'. parse_hex_value
    // must match that exactly -- this pins the fix for a divergence found in
    // review, where a set literal of only " \t\r\n" silently narrowed the
    // accepted whitespace set relative to the original.
    EXPECT_EQ(parse_hex_value("\v0x10"), 0x10u);
    EXPECT_EQ(parse_hex_value("0x10\v"), 0x10u);
    EXPECT_EQ(parse_hex_value("\f0x10"), 0x10u);
    EXPECT_EQ(parse_hex_value("0x10\f"), 0x10u);
    EXPECT_EQ(parse_hex_value("\f0x10\f"), 0x10u);
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
