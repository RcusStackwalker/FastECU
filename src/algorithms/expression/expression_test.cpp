#include "src/algorithms/expression/expression.h"

#include <array>
#include <limits>
#include <string_view>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

namespace fastecu::expression
{
namespace
{
MATCHER_P(ValueIs, matcher, "")
{
    if (!arg.has_value())
    {
        *result_listener << arg.error().detail;
        return false;
    }
    return ::testing::ExplainMatchResult(matcher, *arg, result_listener);
}

MATCHER(HasError, "")
{
    return !arg.has_value() && !arg.error().detail.empty();
}

struct ArithmeticCase
{
    std::string_view expression;
    double input;
    double expected;
};

class CheckedArithmetic : public ::testing::TestWithParam<ArithmeticCase>
{
};

TEST_P(CheckedArithmetic, EvaluatesWithoutTextRounding)
{
    const auto& example = GetParam();
    EXPECT_THAT(EvaluateChecked(example.expression, example.input), ValueIs(::testing::DoubleEq(example.expected)));
}

INSTANTIATE_TEST_SUITE_P(Grammar, CheckedArithmetic,
                         ::testing::Values(ArithmeticCase{"x*2+1", 3, 7}, ArithmeticCase{"-(x+2)", 3, -5},
                                           ArithmeticCase{"+1.25e-2", 3, 0.0125}, ArithmeticCase{"", 3, 3},
                                           ArithmeticCase{" \t\n", 3, 3}, ArithmeticCase{"(2+3)*(4-1)", 0, 15},
                                           ArithmeticCase{"x/-2", 3, -1.5}, ArithmeticCase{"-x+5", 3, 2},
                                           ArithmeticCase{"x*-2", 3, -6}, ArithmeticCase{"--x", 3, 3},
                                           ArithmeticCase{".5+1.", 0, 1.5}, ArithmeticCase{"1E+2", 0, 100},
                                           ArithmeticCase{"1000000000000000+1-1000000000000000", 0, 1}));

class InvalidExpression : public ::testing::TestWithParam<std::string_view>
{
};

TEST_P(InvalidExpression, RejectsWithDiagnostic)
{
    EXPECT_THAT(EvaluateChecked(GetParam(), 2), HasError());
}

INSTANTIATE_TEST_SUITE_P(Grammar, InvalidExpression,
                         ::testing::Values("2junk", "(x", "x)", "()", "x+", "*x", "x x", "y", "sin(x)", "x^2", "1/0",
                                           "0/0", "1e309", "1e308*10", "nan", "inf", "0x10", "1e", "1e+", ".", "1..2"));

TEST(CheckedExpression, RejectsNonFiniteInputs)
{
    EXPECT_THAT(EvaluateChecked("x", std::numeric_limits<double>::infinity()), HasError());
    EXPECT_THAT(EvaluateChecked("1", std::numeric_limits<double>::quiet_NaN()), HasError());
}

TEST(CheckedExpression, ParsesEntireFiniteLiteral)
{
    EXPECT_THAT(ParseFiniteNumber("  -1.25e-2 \n"), ValueIs(::testing::DoubleEq(-0.0125)));
    EXPECT_THAT(ParseFiniteNumber("+20"), ValueIs(20));
    EXPECT_THAT(ParseFiniteNumber("1."), ValueIs(1));
    EXPECT_THAT(ParseFiniteNumber(".5"), ValueIs(0.5));
}

TEST(CheckedExpression, RejectsIncompleteOrNonFiniteLiterals)
{
    for (const auto text :
         std::to_array<std::string_view>({"", " ", "2junk", "nan", "inf", "1e309", "1+2", "0x10", "+-2"}))
    {
        SCOPED_TRACE(text);
        EXPECT_THAT(ParseFiniteNumber(text), HasError());
    }
}

TEST(CheckedExpression, RejectsExcessiveParenthesisNesting)
{
    const std::string text = std::string(512, '(') + "1" + std::string(512, ')');
    EXPECT_THAT(EvaluateChecked(text, 0), HasError());
}
} // namespace
} // namespace fastecu::expression
