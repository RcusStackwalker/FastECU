#include <gtest/gtest.h>

#include "src/algorithms/expression/qt_expression_evaluator.h"

TEST(TestExpressionEvaluator, parse_returnsReversePolishTokens)
{
    ASSERT_EQ(ExpressionEvaluator::parse("(x*2)+10", "4"),
              QStringList({"4", "2", "*", "10", "+"}));
}

TEST(TestExpressionEvaluator, evaluate_appliesOperatorPrecedenceAndParentheses)
{
    ASSERT_EQ(ExpressionEvaluator::evaluate("x*2+10", "4"), 18.0);
    ASSERT_EQ(ExpressionEvaluator::evaluate("(x+10)*2", "4"), 28.0);
}

TEST(TestExpressionEvaluator, evaluate_supportsNegativeInputAndUnaryMinus)
{
    ASSERT_EQ(ExpressionEvaluator::evaluate("-x+5", "3"), 2.0);
    ASSERT_EQ(ExpressionEvaluator::evaluate("x*-2", "3"), -6.0);
}

TEST(TestExpressionEvaluator, evaluate_preservesNanAsZeroCompatibility)
{
    ASSERT_EQ(ExpressionEvaluator::evaluate("0/0", "0"), 0.0);
}

TEST(TestExpressionEvaluator, evaluate_supportsDivision)
{
    ASSERT_DOUBLE_EQ(ExpressionEvaluator::evaluate("8/4", "0"), 2.0);
}

TEST(TestExpressionEvaluator, evaluate_preservesNestedParentheses)
{
    ASSERT_DOUBLE_EQ(ExpressionEvaluator::evaluate("(2+3)*(4-1)", "0"), 15.0);
}
