#ifndef EXPRESSION_EVALUATOR_H
#define EXPRESSION_EVALUATOR_H

#include <QString>
#include <QStringList>

class ExpressionEvaluator
{
  public:
    static QStringList parse(const QString& expression, const QString& x);
    static double evaluate(QStringList expression, int precision = 15);
    static double evaluate(const QString& expression, const QString& x, int precision = 15);
};

#endif // EXPRESSION_EVALUATOR_H
