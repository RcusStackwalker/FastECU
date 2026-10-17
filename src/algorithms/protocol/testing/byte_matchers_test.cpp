#include "src/algorithms/protocol/testing/byte_matchers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

namespace test_bytes
{
namespace
{
using ::testing::HasSubstr;
using ::testing::Not;

TEST(BytesFilledWith, MatchesEmptyAndUniformRanges)
{
    EXPECT_THAT(bytes::Bytes{}, BytesFilledWith(0xFF));
    EXPECT_THAT(bytes::Bytes(4096, 0xA5), BytesFilledWith(0xA5));
    const bytes::Bytes storage{0x00, 0xFF, 0xFF, 0x00};
    EXPECT_THAT(bytes::ByteView(storage).subspan(1, 2), BytesFilledWith(0xFF));
}

TEST(BytesFilledWith, RejectsAMismatchAtEveryPosition)
{
    for (const std::size_t offset : {0U, 2048U, 4095U})
    {
        SCOPED_TRACE(offset);
        bytes::Bytes actual(4096, 0xA5);
        actual[offset] = 0x5A;
        ::testing::StringMatchResultListener listener;
        EXPECT_FALSE(::testing::ExplainMatchResult(BytesFilledWith(0xA5), actual, &listener));
        EXPECT_THAT(listener.str(), HasSubstr(std::to_string(offset)));
        EXPECT_THAT(listener.str(), HasSubstr("5a"));
    }
}

TEST(BytesFilledWith, DoesNotTruncateTheExpectedValue)
{
    EXPECT_THAT(bytes::Bytes{0x00}, Not(BytesFilledWith(0x100)));
}

TEST(BytesEq, MatchesEqualContent)
{
    const bytes::Bytes actual{0x7E, 0x00, 0x27, 0x61};

    EXPECT_THAT(actual, BytesEq(bytes::Bytes{0x7E, 0x00, 0x27, 0x61}));
}

TEST(BytesEq, RejectsDifferentContent)
{
    const bytes::Bytes actual{0x7E, 0x00, 0x27, 0x61};

    EXPECT_THAT(actual, Not(BytesEq(bytes::Bytes{0x7E, 0x00, 0x27, 0x62})));
}

TEST(BytesEq, RejectsADifferentLength)
{
    const bytes::Bytes actual{0x7E, 0x00};

    EXPECT_THAT(actual, Not(BytesEq(bytes::Bytes{0x7E, 0x00, 0x27})));
}

TEST(BytesEq, ExplainsAMismatchAsHexRatherThanDecimalChars)
{
    const bytes::Bytes actual{0xA5, 0x5A};
    ::testing::StringMatchResultListener listener;

    ::testing::ExplainMatchResult(BytesEq(bytes::Bytes{0xA5, 0x5B}), actual, &listener);

    EXPECT_THAT(listener.str(), HasSubstr("a5 5a"));
    EXPECT_THAT(listener.str(), HasSubstr("a5 5b"));
}

TEST(BytesEq, ComparesAViewAgainstAVector)
{
    const bytes::Bytes storage{0x01, 0x02, 0x03};

    EXPECT_THAT(bytes::ByteView(storage), BytesEq(bytes::Bytes{0x01, 0x02, 0x03}));
}
} // namespace
} // namespace test_bytes
