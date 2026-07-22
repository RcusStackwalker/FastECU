#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/diagnostics/nrc_parser.h"

#include <gtest/gtest.h>

#include <unordered_map>

TEST(DiagnosticsPortable, NrcDecodesKnownNegativeResponse)
{
    const std::unordered_map<int, std::string> codes = {{0x31, "Request out of range"}};
    const bytes::Bytes frame = {0x7f, 0x22, 0x31};

    EXPECT_EQ(nrc_description(frame, codes), "Request out of range");
}

TEST(DiagnosticsPortable, NrcRejectsTooShortResponse)
{
    const bytes::Bytes tooShort = {0x62, 0x22};

    EXPECT_EQ(nrc_description(tooShort, {}), "Not a valid answer");
}

TEST(DiagnosticsPortable, NrcUnknownCodeReturnsUnknownErrorCode)
{
    // Real contract, mirrored from tests/test_diagnostic_parsers.cpp's
    // nrc_rejectsMalformedResponse vector: a well-formed negative-response
    // frame (0x7f <sid> <nrc>) whose NRC byte has no entry in the codes
    // table falls back to the literal string "Unknown error code" -- not
    // an empty string, not a placeholder.
    const bytes::Bytes frame = {0x7f, 0x22, 0xff};

    EXPECT_EQ(nrc_description(frame, {}), "Unknown error code");
}

TEST(DiagnosticsPortable, DtcDecodesKnownCategoryMap)
{
    const std::unordered_map<int, std::string> cCodes = {{0x4001, "C0001 - Test chassis code"}};

    EXPECT_EQ(dtc_description(0x4001, {}, cCodes, {}, {}), "C0001 - Test chassis code");
}

TEST(DiagnosticsPortable, DtcUsesCategoryPrefixForUnknownCodes)
{
    EXPECT_EQ(dtc_description(0x0001, {}, {}, {}, {}), "P0001 - Unknown error code");
    EXPECT_EQ(dtc_description(0x4001, {}, {}, {}, {}), "C0001 - Unknown error code");
    EXPECT_EQ(dtc_description(0x8001, {}, {}, {}, {}), "B0001 - Unknown error code");
    EXPECT_EQ(dtc_description(0xc001, {}, {}, {}, {}), "U0001 - Unknown error code");
}
