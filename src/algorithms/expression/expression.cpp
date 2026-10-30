#include "src/algorithms/expression/expression.h"

#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <utility>
#include <vector>

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

struct Instruction
{
    char operation;
    double value;
};
using Constant = std::optional<double>;
using CompileResult = std::expected<Constant, EvaluationError>;

EvaluationResult Apply(char operation, double left, double right)
{
    switch (operation)
    {
    case '+':
        return Finite(left + right);
    case '-':
        return Finite(left - right);
    case '*':
        return Finite(left * right);
    case '/':
        if (right == 0.0)
        {
            return std::unexpected(EvaluationError{"cannot divide by zero"});
        }
        return Finite(left / right);
    default:
        return std::unexpected(EvaluationError{"invalid compiled operation"});
    }
}

class Parser
{
  public:
    Parser(std::string_view text, std::vector<Instruction>& instructions) : text_(text), instructions_(instructions)
    {
    }

    CompileResult Compile()
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
    CompileResult Error(std::string_view detail) const
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

    CompileResult Sum()
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
            left = Binary(operation, *left, *right);
        }
        return left;
    }

    CompileResult Product()
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
            if (operation == '/' && right->has_value() && **right == 0.0)
            {
                return Error("cannot divide by zero");
            }
            left = Binary(operation, *left, *right);
        }
        return left;
    }

    CompileResult Unary()
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
            instructions_.push_back({'~', 0.0});
            if (value->has_value())
            {
                *value = -**value;
            }
        }
        return value;
    }

    CompileResult Primary()
    {
        if (Consume('('))
        {
            constexpr std::size_t kMaximumNesting = 8;
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
            instructions_.push_back({'x', 0.0});
            return Constant{};
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
        const auto finite = Finite(value);
        if (!finite.has_value())
        {
            return std::unexpected(finite.error());
        }
        instructions_.push_back({'n', value});
        return Constant{value};
    }

    CompileResult Binary(char operation, Constant left, Constant right)
    {
        instructions_.push_back({operation, 0.0});
        if (!left.has_value() || !right.has_value())
        {
            return Constant{};
        }
        const auto value = Apply(operation, *left, *right);
        if (!value.has_value())
        {
            return std::unexpected(value.error());
        }
        return Constant{*value};
    }

    std::string_view text_;
    std::vector<Instruction>& instructions_;
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

struct CompiledExpression::Program
{
    std::vector<Instruction> instructions;
};

CompiledExpression::CompiledExpression(std::shared_ptr<const Program> program) : program_(std::move(program))
{
}

std::expected<CompiledExpression, EvaluationError> Compile(std::string_view expression)
{
    auto program = std::make_shared<CompiledExpression::Program>();
    expression = Trimmed(expression);
    if (expression.empty())
    {
        program->instructions.push_back({'x', 0.0});
    }
    else
    {
        const auto result = Parser(expression, program->instructions).Compile();
        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }
    }
    return CompiledExpression(std::move(program));
}

std::expected<double, EvaluationError> CompiledExpression::Evaluate(double x) const
{
    const auto input = Finite(x);
    if (!input.has_value())
    {
        return input;
    }
    std::vector<double> values;
    values.reserve(program_->instructions.size());
    for (const auto& instruction : program_->instructions)
    {
        if (instruction.operation == 'n' || instruction.operation == 'x')
        {
            values.push_back(instruction.operation == 'x' ? x : instruction.value);
        }
        else if (instruction.operation == '~')
        {
            values.back() = -values.back();
        }
        else
        {
            const double right = values.back();
            values.pop_back();
            const auto value = Apply(instruction.operation, values.back(), right);
            if (!value.has_value())
            {
                return value;
            }
            values.back() = *value;
        }
    }
    return values.back();
}

std::expected<double, EvaluationError> EvaluateChecked(std::string_view expression, double x)
{
    return Compile(expression).and_then([&](const CompiledExpression& compiled) { return compiled.Evaluate(x); });
}
} // namespace fastecu::expression
