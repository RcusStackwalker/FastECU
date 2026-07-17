#include "expression_evaluator.h"

#include <cmath>

namespace
{

int precedence(const QString& op)
{
    if (op == "*" || op == "/")
    {
        return 2;
    }
    if (op == "+" || op == "-")
    {
        return 1;
    }
    return 0;
}

bool shouldPopBefore(const QStringList& operators, const QString& nextOperator)
{
    return !operators.isEmpty() && operators.last() != "(" && precedence(operators.last()) >= precedence(nextOperator);
}

QString normalizedSingleValue(QString value)
{
    if (value.startsWith("--"))
    {
        value.remove(0, 2);
    }
    return value;
}

} // namespace

QStringList ExpressionEvaluator::parse(const QString& expression, const QString& x)
{
    QStringList numbers;
    QStringList operators;
    bool isOperator = true;

    int i = 0;
    while (i < expression.length())
    {
        const QChar ch = expression.at(i);
        QString number;

        if (ch.isSpace())
        {
            i++;
            continue;
        }

        if (ch == 'x')
        {
            isOperator = false;
            numbers.append(x);
        }
        else if (isOperator && ch == '-' && i + 1 < expression.length() && expression.at(i + 1) == 'x')
        {
            isOperator = false;
            numbers.append("-" + x);
            i++;
        }
        else if (ch.isNumber() || ch == '.' || (isOperator && ch == '-'))
        {
            isOperator = false;
            number.append(ch);
            i++;
            while (i < expression.length() && (expression.at(i).isNumber() || expression.at(i) == '.'))
            {
                number.append(expression.at(i));
                i++;
            }
            i--;
            numbers.append(number);
        }
        else if (ch == '(')
        {
            isOperator = true;
            operators.append(ch);
        }
        else if (ch == ')')
        {
            while (!operators.isEmpty() && operators.last() != "(")
            {
                numbers.append(operators.takeLast());
            }

            if (!operators.isEmpty())
            {
                operators.removeLast();
            }
        }
        else if (ch == '*' || ch == '/')
        {
            isOperator = true;
            while (shouldPopBefore(operators, ch))
            {
                numbers.append(operators.takeLast());
            }
            operators.append(ch);
        }
        else if (ch == '+' || ch == '-')
        {
            isOperator = true;
            while (shouldPopBefore(operators, ch))
            {
                numbers.append(operators.takeLast());
            }
            operators.append(ch);
        }
        i++;
    }

    while (!operators.isEmpty())
    {
        numbers.append(operators.takeLast());
    }

    return numbers;
}

double ExpressionEvaluator::evaluate(QStringList expression, int precision)
{
    double value = 0;

    if (expression.length() == 1)
    {
        value = normalizedSingleValue(expression.at(0)).toDouble();
    }

    while (expression.length() > 1)
    {
        bool reduced = false;
        for (int i = 0; i < expression.length(); i++)
        {
            if (i < 2)
            {
                continue;
            }

            if (expression.at(i) == "-")
            {
                value = expression.at(i - 2).toDouble() - expression.at(i - 1).toDouble();
            }
            else if (expression.at(i) == "+")
            {
                value = expression.at(i - 2).toDouble() + expression.at(i - 1).toDouble();
            }
            else if (expression.at(i) == "*")
            {
                value = expression.at(i - 2).toDouble() * expression.at(i - 1).toDouble();
            }
            else if (expression.at(i) == "/")
            {
                value = expression.at(i - 2).toDouble() / expression.at(i - 1).toDouble();
            }
            else
            {
                continue;
            }

            expression.replace(i, QString::number(value, 'g', precision));
            expression.removeAt(i - 1);
            expression.removeAt(i - 2);
            reduced = true;
            break;
        }

        if (!reduced)
        {
            break;
        }
    }

    if (std::isnan(value))
    {
        value = 0;
    }
    return value;
}

double ExpressionEvaluator::evaluate(const QString& expression, const QString& x, int precision)
{
    return evaluate(parse(expression, x), precision);
}
