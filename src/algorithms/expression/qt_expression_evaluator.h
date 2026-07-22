#ifndef QT_EXPRESSION_EVALUATOR_H
#define QT_EXPRESSION_EVALUATOR_H

#include "src/algorithms/expression/expression_evaluator.h"

#include <QString>
#include <QStringList>

// TRANSITIONAL Qt shim. Preserves the pre-conversion ExpressionEvaluator
// static-method API (QString/QStringList) for callers not yet converted,
// delegating to the portable expression_parse()/expression_evaluate() free
// functions in expression_evaluator.h. C++ cannot add member functions to
// an existing class from a second header, so unlike menu_command's free
// function overloads, this keeps the whole class here rather than only the
// Qt-typed overloads.
class ExpressionEvaluator
{
  public:
    static QStringList parse(const QString& expression, const QString& x);
    static double evaluate(QStringList expression, int precision = 15);
    static double evaluate(const QString& expression, const QString& x, int precision = 15);
};

#endif // QT_EXPRESSION_EVALUATOR_H
