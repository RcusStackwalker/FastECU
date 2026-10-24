#include "src/backend/definition/text_format.h"

#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::Eq;

namespace fastecu::definition
{
namespace
{

TEST(ParseHexBytesTest, DecodesPairsOfHexDigitsInEitherCase)
{
    EXPECT_EQ(ParseHexBytes("0aFf10"), (bytes::Bytes{0x0A, 0xFF, 0x10}));
    EXPECT_EQ(ParseHexBytes(""), bytes::Bytes{});
}

TEST(ParseHexBytesTest, RejectsAnythingButWholeBareBytes)
{
    for (const std::string_view text : {"0", "070", "0x07", "0Z", " 7", "7 ", "-1", "+1", "07 08"})
    {
        SCOPED_TRACE(::testing::PrintToString(text));
        EXPECT_THAT(ParseHexBytes(text), Eq(std::nullopt)) << text;
    }
}

TEST(ParseHexValueTest, AcceptsPrefixedAndBareHex)
{
    EXPECT_EQ(ParseHexValue("0xFFFF6004"), 0xFFFF6004U);
    EXPECT_EQ(ParseHexValue("0XFFFF3000"), 0xFFFF3000U);
    EXPECT_EQ(ParseHexValue("ffff4000"), 0xFFFF4000U);
    EXPECT_EQ(ParseHexValue("0"), 0U);
}

TEST(ParseHexValueTest, TrimsSurroundingWhitespace)
{
    EXPECT_EQ(ParseHexValue("  0xFFFF6004  "), 0xFFFF6004U);
}

TEST(ParseHexValueTest, TrimsVerticalTabAndFormFeed)
{
    // std::isspace (the original trim_copy's basis) matches '\v' and '\f' in
    // the C locale in addition to ' ', '\t', '\r', '\n'. parse_hex_value
    // must match that exactly -- this pins the fix for a divergence found in
    // review, where a set literal of only " \t\r\n" silently narrowed the
    // accepted whitespace set relative to the original.
    EXPECT_EQ(ParseHexValue("\v0x10"), 0x10U);
    EXPECT_EQ(ParseHexValue("0x10\v"), 0x10U);
    EXPECT_EQ(ParseHexValue("\f0x10"), 0x10U);
    EXPECT_EQ(ParseHexValue("0x10\f"), 0x10U);
    EXPECT_EQ(ParseHexValue("\f0x10\f"), 0x10U);
}

TEST(ParseHexValueTest, RejectsMalformedInput)
{
    EXPECT_THAT(ParseHexValue(""), Eq(std::nullopt));
    EXPECT_THAT(ParseHexValue("0x"), Eq(std::nullopt));
    EXPECT_THAT(ParseHexValue("nonsense"), Eq(std::nullopt));
    EXPECT_THAT(ParseHexValue("0xFFFF6004xyz"), Eq(std::nullopt)); // trailing junk
    EXPECT_THAT(ParseHexValue("-1"), Eq(std::nullopt));
}

TEST(ParseHexValueTest, RoundTripsWithHexText)
{
    EXPECT_EQ(ParseHexValue(HexText(0xDEADBEEFU)), 0xDEADBEEFU);
}

} // namespace
} // namespace fastecu::definition
