#include "src/algorithms/protocol/testing/byte_matchers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace test_bytes
{
namespace
{
using ::testing::HasSubstr;
using ::testing::Not;

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
