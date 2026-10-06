#include "src/algorithms/expression/checked_expression.h"

#include <charconv>
#include <cmath>
#include <format>

namespace fastecu::expression
{
namespace
{
using EvaluationResult = std::expected<double, EvaluationError>;

bool whitespace(char character)
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' ||
           character == '\v';
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && whitespace(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && whitespace(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

EvaluationResult finite(double value)
{
    if (!std::isfinite(value))
    {
        return std::unexpected(EvaluationError{"numeric value is not finite"});
    }
    return value;
}

class Parser
{
  public:
    Parser(std::string_view text, double input) : text_(text), input_(input)
    {
    }

    EvaluationResult evaluate()
    {
        auto value = sum();
        if (!value.has_value())
        {
            return value;
        }
        skip_whitespace();
        if (position_ != text_.size())
        {
            return error("unexpected token");
        }
        return value;
    }

  private:
    EvaluationResult error(std::string_view detail) const
    {
        return std::unexpected(EvaluationError{std::format("{} at position {}", detail, position_)});
    }

    void skip_whitespace()
    {
        while (position_ < text_.size() && whitespace(text_[position_]))
        {
            ++position_;
        }
    }

    bool consume(char token)
    {
        skip_whitespace();
        if (position_ == text_.size() || text_[position_] != token)
        {
            return false;
        }
        ++position_;
        return true;
    }

    EvaluationResult sum()
    {
        auto left = product();
        while (left.has_value())
        {
            skip_whitespace();
            if (position_ == text_.size() || (text_[position_] != '+' && text_[position_] != '-'))
            {
                break;
            }
            const char operation = text_[position_++];
            const auto right = product();
            if (!right.has_value())
            {
                return right;
            }
            left = finite(operation == '+' ? *left + *right : *left - *right);
        }
        return left;
    }

    EvaluationResult product()
    {
        auto left = unary();
        while (left.has_value())
        {
            skip_whitespace();
            if (position_ == text_.size() || (text_[position_] != '*' && text_[position_] != '/'))
            {
                break;
            }
            const char operation = text_[position_++];
            const auto right = unary();
            if (!right.has_value())
            {
                return right;
            }
            if (operation == '/' && *right == 0.0)
            {
                return error("cannot divide by zero");
            }
            left = finite(operation == '*' ? *left * *right : *left / *right);
        }
        return left;
    }

    EvaluationResult unary()
    {
        bool negative = false;
        for (;;)
        {
            if (consume('-'))
            {
                negative = !negative;
            }
            else if (!consume('+'))
            {
                break;
            }
        }
        auto value = primary();
        if (value.has_value() && negative)
        {
            *value = -*value;
        }
        return value;
    }

    EvaluationResult primary()
    {
        if (consume('('))
        {
            constexpr std::size_t kMaximumNesting = 128;
            if (nesting_ >= kMaximumNesting)
            {
                return error("expression nesting is too deep");
            }
            ++nesting_;
            auto value = sum();
            --nesting_;
            if (!value.has_value())
            {
                return value;
            }
            if (!consume(')'))
            {
                return error("missing closing parenthesis");
            }
            return value;
        }
        if (consume('x'))
        {
            return input_;
        }
        skip_whitespace();
        if (position_ == text_.size() ||
            (text_[position_] != '.' && (text_[position_] < '0' || text_[position_] > '9')))
        {
            return error("expected a number, x, or parenthesized expression");
        }
        double value = 0.0;
        const char *begin = text_.data() + position_;
        const char *end = text_.data() + text_.size();
        const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
        if (parsed.ec != std::errc{})
        {
            return error("invalid numeric literal");
        }
        position_ = static_cast<std::size_t>(parsed.ptr - text_.data());
        return finite(value);
    }

    std::string_view text_;
    double input_;
    std::size_t position_{0};
    std::size_t nesting_{0};
};
} // namespace

std::expected<double, EvaluationError> parse_finite_number(std::string_view text)
{
    text = trimmed(text);
    if (!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
        if (!text.empty() && (text.front() == '-' || text.front() == '+'))
        {
            return std::unexpected(EvaluationError{"numeric literal has multiple signs"});
        }
    }
    if (text.empty())
    {
        return std::unexpected(EvaluationError{"numeric literal is empty"});
    }
    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return std::unexpected(EvaluationError{"invalid numeric literal"});
    }
    return finite(value);
}

std::expected<double, EvaluationError> evaluate_checked(std::string_view expression, double x)
{
    const auto input = finite(x);
    if (!input.has_value())
    {
        return input;
    }
    expression = trimmed(expression);
    if (expression.empty())
    {
        return x;
    }
    return Parser(expression, x).evaluate();
}
} // namespace fastecu::expression
