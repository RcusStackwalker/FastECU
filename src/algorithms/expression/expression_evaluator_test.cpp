#include "src/algorithms/expression/expression_evaluator.h"

#include <gtest/gtest.h>

TEST(ExpressionEvaluatorPortable, ParseReturnsReversePolishTokens)
{
    const std::vector<std::string> expected = {"4", "2", "*", "10", "+"};
    EXPECT_EQ(ExpressionParse("(x*2)+10", "4"), expected);
}

TEST(ExpressionEvaluatorPortable, EvaluateAppliesOperatorPrecedenceAndParentheses)
{
    EXPECT_EQ(ExpressionEvaluate("x*2+10", "4"), 18.0);
    EXPECT_EQ(ExpressionEvaluate("(x+10)*2", "4"), 28.0);
}

TEST(ExpressionEvaluatorPortable, EvaluateSupportsNegativeInputAndUnaryMinus)
{
    EXPECT_EQ(ExpressionEvaluate("-x+5", "3"), 2.0);
    EXPECT_EQ(ExpressionEvaluate("x*-2", "3"), -6.0);
}

TEST(ExpressionEvaluatorPortable, EvaluatePreservesNanAsZeroCompatibility)
{
    EXPECT_EQ(ExpressionEvaluate("0/0", "0"), 0.0);
}

TEST(ExpressionEvaluatorPortable, EvaluateSupportsDivision)
{
    EXPECT_DOUBLE_EQ(ExpressionEvaluate("8/4", "0"), 2.0);
}

TEST(ExpressionEvaluatorPortable, EvaluatePreservesNestedParentheses)
{
    EXPECT_DOUBLE_EQ(ExpressionEvaluate("(2+3)*(4-1)", "0"), 15.0);
}
