#include "src/backend/ports/testing/result_matchers.h"

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::testing
{
namespace
{
using ::testing::AllOf;
using ::testing::DescribeMatcher;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::StrEq;

// NOLINTBEGIN(fastecu-result-assertion): these test the negation of IsOk itself, so IsErr would stop
// exercising the matcher contract (for Status, independently of error classification).
TEST(ResultMatchers, IsOkAcceptsAValueAndRejectsAnError)
{
    const Result<int> good = 7;
    const Result<int> bad = Fail(ErrorKind::kTimeout, "no reply");

    EXPECT_THAT(good, IsOk());
    EXPECT_THAT(bad, Not(IsOk()));
}

TEST(ResultMatchers, IsOkAcceptsAStatus)
{
    const Status good{};
    const Status bad = Fail(ErrorKind::kDisconnected);

    EXPECT_THAT(good, IsOk());
    EXPECT_THAT(bad, Not(IsOk()));
}
// NOLINTEND(fastecu-result-assertion)

TEST(ResultMatchers, IsOkAndInspectsTheValue)
{
    const Result<int> good = 7;

    EXPECT_THAT(good, IsOkAnd(7));
    EXPECT_THAT(good, Not(IsOkAnd(8)));
}

TEST(ResultMatchers, IsErrRequiresTheExactKind)
{
    const Result<int> bad = Fail(ErrorKind::kTimeout, "no reply");

    EXPECT_THAT(bad, IsErr(ErrorKind::kTimeout));
    EXPECT_THAT(bad, Not(IsErr(ErrorKind::kBadResponse)));
}

TEST(ResultMatchers, IsErrWithAlsoMatchesTheDetail)
{
    const Result<int> bad = Fail(ErrorKind::kInvalidConfig, "plan does not match this executor");

    EXPECT_THAT(bad, IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("does not match this executor")));
    EXPECT_THAT(bad, Not(IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("wrong MCU"))));
}

// The whole point of the matchers: a failure has to say what the error was.
TEST(ResultMatchers, AFailedIsOkExplainsTheErrorItFound)
{
    const Result<int> bad = Fail(ErrorKind::kTimeout, "no reply");

    EXPECT_THAT(::testing::DescribeMatcher<Result<int>>(IsOk()), HasSubstr("ok"));
    EXPECT_THAT(ExplainMatch(IsOk(), bad), HasSubstr("Timeout"));
    EXPECT_THAT(ExplainMatch(IsOk(), bad), HasSubstr("no reply"));
}

TEST(ResultMatchers, AFailedIsErrExplainsTheKindItFoundInstead)
{
    const Result<int> bad = Fail(ErrorKind::kTimeout, "no reply");

    EXPECT_THAT(ExplainMatch(IsErr(ErrorKind::kBadResponse), bad), HasSubstr("Timeout"));
}

// Expected: lines read as words; a matcher parameter never prints as bytes.
TEST(ResultMatchers, DescribesEachMatcherInWords)
{
    EXPECT_EQ(DescribeMatcher<Result<int>>(IsOk()), "is ok");
    EXPECT_EQ(DescribeMatcher<Status>(Not(IsOk())), "is not ok");
    EXPECT_EQ(DescribeMatcher<Result<int>>(IsOkAnd(42)), "is ok and its value is equal to 42");
    EXPECT_EQ(DescribeMatcher<Result<int>>(Not(IsOkAnd(42))), "is not ok, or its value isn't equal to 42");
    EXPECT_EQ(DescribeMatcher<Result<int>>(IsErr(ErrorKind::kTimeout)), "is an error of kind Timeout");
    EXPECT_EQ(DescribeMatcher<Result<int>>(Not(IsErr(ErrorKind::kTimeout))), "is not an error of kind Timeout");
    EXPECT_EQ(DescribeMatcher<Result<int>>(IsErrWith(ErrorKind::kTimeout, StrEq("no reply"))),
              "is an error of kind Timeout whose detail is equal to \"no reply\"");
    EXPECT_EQ(DescribeMatcher<Result<int>>(Not(IsErrWith(ErrorKind::kTimeout, StrEq("no reply")))),
              "is not an error of kind Timeout whose detail is equal to \"no reply\"");
    // Eq on a string literal keeps gmock's own wording ("0x... pointing to");
    // the matcher itself must still print as text, not as its bytes.
    EXPECT_THAT(DescribeMatcher<Result<int>>(IsErrWith(ErrorKind::kTimeout, Eq("no reply"))),
                AllOf(HasSubstr("\"no reply\""), Not(HasSubstr("-byte object"))));
}

TEST(ResultMatchers, AMismatchedDetailExplainsTheDetailItFound)
{
    const Result<int> bad = Fail(ErrorKind::kTimeout, "no reply");

    EXPECT_THAT(ExplainMatch(IsErrWith(ErrorKind::kTimeout, Eq("x")), bad), HasSubstr("whose detail is \"no reply\""));
}

TEST(ResultMatchers, AMismatchedValueExplainsTheValueItFound)
{
    const Result<int> good = 2;

    EXPECT_THAT(ExplainMatch(IsOkAnd(1), good), HasSubstr("whose value is 2"));
}
} // namespace
} // namespace fastecu::testing
