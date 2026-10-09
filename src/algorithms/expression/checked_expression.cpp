#include "src/algorithms/expression/checked_expression.h"

#include <charconv>
#include <cmath>
#include <format>

namespace fastecu::expression
{
namespace
{
using EvaluationResult = std::expected<double, EvaluationError>;

bool Whitespace(char character)
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' ||
           character == '\v';
}

std::string_view Trimmed(std::string_view text)
{
    while (!text.empty() && Whitespace(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && Whitespace(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

EvaluationResult Finite(double value)
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

    EvaluationResult Evaluate()
    {
        auto value = Sum();
        if (!value.has_value())
        {
            return value;
        }
        SkipWhitespace();
        if (position_ != text_.size())
        {
            return Error("unexpected token");
        }
        return value;
    }

  private:
    EvaluationResult Error(std::string_view detail) const
    {
        return std::unexpected(EvaluationError{std::format("{} at position {}", detail, position_)});
    }

    void SkipWhitespace()
    {
        while (position_ < text_.size() && Whitespace(text_[position_]))
        {
            ++position_;
        }
    }

    bool Consume(char token)
    {
        SkipWhitespace();
        if (position_ == text_.size() || text_[position_] != token)
        {
            return false;
        }
        ++position_;
        return true;
    }

    EvaluationResult Sum()
    {
        auto left = Product();
        while (left.has_value())
        {
            SkipWhitespace();
            if (position_ == text_.size() || (text_[position_] != '+' && text_[position_] != '-'))
            {
                break;
            }
            const char operation = text_[position_++];
            const auto right = Product();
            if (!right.has_value())
            {
                return right;
            }
            left = Finite(operation == '+' ? *left + *right : *left - *right);
        }
        return left;
    }

    EvaluationResult Product()
    {
        auto left = Unary();
        while (left.has_value())
        {
            SkipWhitespace();
            if (position_ == text_.size() || (text_[position_] != '*' && text_[position_] != '/'))
            {
                break;
            }
            const char operation = text_[position_++];
            const auto right = Unary();
            if (!right.has_value())
            {
                return right;
            }
            if (operation == '/' && *right == 0.0)
            {
                return Error("cannot divide by zero");
            }
            left = Finite(operation == '*' ? *left * *right : *left / *right);
        }
        return left;
    }

    EvaluationResult Unary()
    {
        bool negative = false;
        for (;;)
        {
            if (Consume('-'))
            {
                negative = !negative;
            }
            else if (!Consume('+'))
            {
                break;
            }
        }
        auto value = Primary();
        if (value.has_value() && negative)
        {
            *value = -*value;
        }
        return value;
    }

    EvaluationResult Primary()
    {
        if (Consume('('))
        {
            constexpr std::size_t kMaximumNesting = 128;
            if (nesting_ >= kMaximumNesting)
            {
                return Error("expression nesting is too deep");
            }
            ++nesting_;
            auto value = Sum();
            --nesting_;
            if (!value.has_value())
            {
                return value;
            }
            if (!Consume(')'))
            {
                return Error("missing closing parenthesis");
            }
            return value;
        }
        if (Consume('x'))
        {
            return input_;
        }
        SkipWhitespace();
        if (position_ == text_.size() ||
            (text_[position_] != '.' && (text_[position_] < '0' || text_[position_] > '9')))
        {
            return Error("expected a number, x, or parenthesized expression");
        }
        double value = 0.0;
        const char *begin = text_.data() + position_;
        const char *end = text_.data() + text_.size();
        const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
        if (parsed.ec != std::errc{})
        {
            return Error("invalid numeric literal");
        }
        position_ = static_cast<std::size_t>(parsed.ptr - text_.data());
        return Finite(value);
    }

    std::string_view text_;
    double input_;
    std::size_t position_{0};
    std::size_t nesting_{0};
};
} // namespace

std::expected<double, EvaluationError> ParseFiniteNumber(std::string_view text)
{
    text = Trimmed(text);
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
    return Finite(value);
}

std::expected<double, EvaluationError> EvaluateChecked(std::string_view expression, double x)
{
    const auto input = Finite(x);
    if (!input.has_value())
    {
        return input;
    }
    expression = Trimmed(expression);
    if (expression.empty())
    {
        return x;
    }
    return Parser(expression, x).Evaluate();
}
} // namespace fastecu::expression
