#include "src/algorithms/expression/qt_expression_evaluator.h"

#include <utility>

QStringList ExpressionEvaluator::parse(const QString& expression, const QString& x)
{
    const std::vector<std::string> tokens = expression_parse(expression.toStdString(), x.toStdString());

    QStringList result;
    result.reserve(static_cast<int>(tokens.size()));
    for (const std::string& token : tokens)
    {
        result.append(QString::fromStdString(token));
    }
    return result;
}

double ExpressionEvaluator::evaluate(QStringList expression, int precision)
{
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<size_t>(expression.size()));
    for (const QString& token : expression)
    {
        tokens.push_back(token.toStdString());
    }
    return expression_evaluate(std::move(tokens), precision);
}

double ExpressionEvaluator::evaluate(const QString& expression, const QString& x, int precision)
{
    return expression_evaluate(expression.toStdString(), x.toStdString(), precision);
}
