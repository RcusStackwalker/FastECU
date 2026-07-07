#include <QtTest>

#include "expression_evaluator.h"
#include "test_expression_evaluator.h"

class TestExpressionEvaluator : public QObject
{
    Q_OBJECT

  private slots:
    void parse_returnsReversePolishTokens()
    {
        QCOMPARE(ExpressionEvaluator::parse("(x*2)+10", "4"),
                 QStringList({"4", "2", "*", "10", "+"}));
    }

    void evaluate_appliesOperatorPrecedenceAndParentheses()
    {
        QCOMPARE(ExpressionEvaluator::evaluate("x*2+10", "4"), 18.0);
        QCOMPARE(ExpressionEvaluator::evaluate("(x+10)*2", "4"), 28.0);
    }

    void evaluate_supportsNegativeInputAndUnaryMinus()
    {
        QCOMPARE(ExpressionEvaluator::evaluate("-x+5", "3"), 2.0);
        QCOMPARE(ExpressionEvaluator::evaluate("x*-2", "3"), -6.0);
    }

    void evaluate_preservesNanAsZeroCompatibility()
    {
        QCOMPARE(ExpressionEvaluator::evaluate("0/0", "0"), 0.0);
    }
};

int run_test_expression_evaluator(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestExpressionEvaluator t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_expression_evaluator.moc"
