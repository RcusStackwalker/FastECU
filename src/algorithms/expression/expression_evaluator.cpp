#include "src/algorithms/expression/expression_evaluator.h"

#include <cctype>
#include <cmath>
#include <exception>
#include <format>

namespace
{

int precedence(const std::string& op)
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

bool shouldPopBefore(const std::vector<std::string>& operators, const std::string& next_operator)
{
    return !operators.empty() && operators.back() != "(" && precedence(operators.back()) >= precedence(next_operator);
}

std::string normalizedSingleValue(std::string value)
{
    if (value.rfind("--", 0) == 0)
    {
        value.erase(0, 2);
    }
    return value;
}

double toDouble(const std::string& value)
{
    try
    {
        size_t consumed = 0;
        const double result = std::stod(value, &consumed);
        return result;
    }
    catch (const std::exception&)
    {
        return 0.0;
    }
}

std::string formatNumber(double value, int precision)
{
    if (precision < 1)
    {
        precision = 1;
    }
    return std::format("{:.{}g}", value, precision);
}

} // namespace

std::vector<std::string> expression_parse(std::string_view expression, std::string_view x)
{
    std::vector<std::string> numbers;
    std::vector<std::string> operators;
    bool is_operator = true;

    size_t i = 0;
    while (i < expression.length())
    {
        const char ch = expression[i];
        std::string number;

        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            i++;
            continue;
        }

        if (ch == 'x')
        {
            is_operator = false;
            numbers.emplace_back(x);
        }
        else if (is_operator && ch == '-' && i + 1 < expression.length() && expression[i + 1] == 'x')
        {
            is_operator = false;
            numbers.push_back(std::format("-{}", x));
            i++;
        }
        else if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || (is_operator && ch == '-'))
        {
            is_operator = false;
            number.push_back(ch);
            i++;
            while (i < expression.length() &&
                   (std::isdigit(static_cast<unsigned char>(expression[i])) || expression[i] == '.'))
            {
                number.push_back(expression[i]);
                i++;
            }
            i--;
            numbers.push_back(number);
        }
        else if (ch == '(')
        {
            is_operator = true;
            operators.emplace_back(1, ch);
        }
        else if (ch == ')')
        {
            while (!operators.empty() && operators.back() != "(")
            {
                numbers.push_back(operators.back());
                operators.pop_back();
            }

            if (!operators.empty())
            {
                operators.pop_back();
            }
        }
        else if (ch == '*' || ch == '/')
        {
            is_operator = true;
            const std::string op_str(1, ch);
            while (shouldPopBefore(operators, op_str))
            {
                numbers.push_back(operators.back());
                operators.pop_back();
            }
            operators.push_back(op_str);
        }
        else if (ch == '+' || ch == '-')
        {
            is_operator = true;
            const std::string op_str(1, ch);
            while (shouldPopBefore(operators, op_str))
            {
                numbers.push_back(operators.back());
                operators.pop_back();
            }
            operators.push_back(op_str);
        }
        i++;
    }

    while (!operators.empty())
    {
        numbers.push_back(operators.back());
        operators.pop_back();
    }

    return numbers;
}

double expression_evaluate(std::vector<std::string> expression, int precision)
{
    double value = 0;

    if (expression.size() == 1)
    {
        value = toDouble(normalizedSingleValue(expression[0]));
    }

    while (expression.size() > 1)
    {
        bool reduced = false;
        for (size_t i = 0; i < expression.size(); i++)
        {
            if (i < 2)
            {
                continue;
            }

            if (expression[i] == "-")
            {
                value = toDouble(expression[i - 2]) - toDouble(expression[i - 1]);
            }
            else if (expression[i] == "+")
            {
                value = toDouble(expression[i - 2]) + toDouble(expression[i - 1]);
            }
            else if (expression[i] == "*")
            {
                value = toDouble(expression[i - 2]) * toDouble(expression[i - 1]);
            }
            else if (expression[i] == "/")
            {
                value = toDouble(expression[i - 2]) / toDouble(expression[i - 1]);
            }
            else
            {
                continue;
            }

            expression[i] = formatNumber(value, precision);
            expression.erase(expression.begin() + static_cast<long>(i) - 1);
            expression.erase(expression.begin() + static_cast<long>(i) - 2);
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

double expression_evaluate(std::string_view expression, std::string_view x, int precision)
{
    return expression_evaluate(expression_parse(expression, x), precision);
}
