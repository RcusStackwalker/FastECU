#include "src/backend/ports/testing/result_matchers.h"

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::testing
{
namespace
{
using ::testing::HasSubstr;
using ::testing::Not;

TEST(ResultMatchers, IsOkAcceptsAValueAndRejectsAnError)
{
    const Result<int> good = 7;
    const Result<int> bad = fail(ErrorKind::Timeout, "no reply");

    EXPECT_THAT(good, IsOk());
    EXPECT_THAT(bad, Not(IsOk()));
}

TEST(ResultMatchers, IsOkAcceptsAStatus)
{
    const Status good{};
    const Status bad = fail(ErrorKind::Disconnected);

    EXPECT_THAT(good, IsOk());
    EXPECT_THAT(bad, Not(IsOk()));
}

TEST(ResultMatchers, IsOkAndInspectsTheValue)
{
    const Result<int> good = 7;

    EXPECT_THAT(good, IsOkAnd(7));
    EXPECT_THAT(good, Not(IsOkAnd(8)));
}

TEST(ResultMatchers, IsErrRequiresTheExactKind)
{
    const Result<int> bad = fail(ErrorKind::Timeout, "no reply");

    EXPECT_THAT(bad, IsErr(ErrorKind::Timeout));
    EXPECT_THAT(bad, Not(IsErr(ErrorKind::BadResponse)));
}

TEST(ResultMatchers, IsErrWithAlsoMatchesTheDetail)
{
    const Result<int> bad = fail(ErrorKind::InvalidConfig, "plan does not match this executor");

    EXPECT_THAT(bad, IsErrWith(ErrorKind::InvalidConfig, HasSubstr("does not match this executor")));
    EXPECT_THAT(bad, Not(IsErrWith(ErrorKind::InvalidConfig, HasSubstr("wrong MCU"))));
}

// The whole point of the matchers: a failure has to say what the error was.
TEST(ResultMatchers, AFailedIsOkExplainsTheErrorItFound)
{
    const Result<int> bad = fail(ErrorKind::Timeout, "no reply");

    EXPECT_THAT(::testing::DescribeMatcher<Result<int>>(IsOk()), HasSubstr("ok"));
    EXPECT_THAT(explainMatch(IsOk(), bad), HasSubstr("Timeout"));
    EXPECT_THAT(explainMatch(IsOk(), bad), HasSubstr("no reply"));
}

TEST(ResultMatchers, AFailedIsErrExplainsTheKindItFoundInstead)
{
    const Result<int> bad = fail(ErrorKind::Timeout, "no reply");

    EXPECT_THAT(explainMatch(IsErr(ErrorKind::BadResponse), bad), HasSubstr("Timeout"));
}
} // namespace
} // namespace fastecu::testing
