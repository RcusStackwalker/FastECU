#include "src/backend/definition/text_format.h"

#include <string_view>

#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{

TEST(ParseHexBytesTest, DecodesPairsOfHexDigitsInEitherCase)
{
    EXPECT_EQ(parse_hex_bytes("0aFf10"), (bytes::Bytes{0x0A, 0xFF, 0x10}));
    EXPECT_EQ(parse_hex_bytes(""), bytes::Bytes{});
}

TEST(ParseHexBytesTest, RejectsAnythingButWholeBareBytes)
{
    for (const std::string_view text : {"0", "070", "0x07", "0Z", " 7", "7 ", "-1", "+1", "07 08"})
    {
        EXPECT_FALSE(parse_hex_bytes(text).has_value()) << text;
    }
}

TEST(ParseHexValueTest, AcceptsPrefixedAndBareHex)
{
    EXPECT_EQ(parse_hex_value("0xFFFF6004"), 0xFFFF6004U);
    EXPECT_EQ(parse_hex_value("0XFFFF3000"), 0xFFFF3000U);
    EXPECT_EQ(parse_hex_value("ffff4000"), 0xFFFF4000U);
    EXPECT_EQ(parse_hex_value("0"), 0U);
}

TEST(ParseHexValueTest, TrimsSurroundingWhitespace)
{
    EXPECT_EQ(parse_hex_value("  0xFFFF6004  "), 0xFFFF6004U);
}

TEST(ParseHexValueTest, TrimsVerticalTabAndFormFeed)
{
    // std::isspace (the original trim_copy's basis) matches '\v' and '\f' in
    // the C locale in addition to ' ', '\t', '\r', '\n'. parse_hex_value
    // must match that exactly -- this pins the fix for a divergence found in
    // review, where a set literal of only " \t\r\n" silently narrowed the
    // accepted whitespace set relative to the original.
    EXPECT_EQ(parse_hex_value("\v0x10"), 0x10U);
    EXPECT_EQ(parse_hex_value("0x10\v"), 0x10U);
    EXPECT_EQ(parse_hex_value("\f0x10"), 0x10U);
    EXPECT_EQ(parse_hex_value("0x10\f"), 0x10U);
    EXPECT_EQ(parse_hex_value("\f0x10\f"), 0x10U);
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
    EXPECT_EQ(parse_hex_value(hex_text(0xDEADBEEFU)), 0xDEADBEEFU);
}

} // namespace
} // namespace fastecu::definition
