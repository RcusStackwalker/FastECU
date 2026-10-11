#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace fastecu::expression
{
struct EvaluationError
{
    std::string detail;
};

class CompiledExpression
{
  public:
    std::expected<double, EvaluationError> Evaluate(double x) const;

  private:
    struct Program;
    explicit CompiledExpression(std::shared_ptr<const Program> program);
    std::shared_ptr<const Program> program_;
    friend std::expected<CompiledExpression, EvaluationError> Compile(std::string_view expression);
};

std::expected<CompiledExpression, EvaluationError> Compile(std::string_view expression);
std::expected<double, EvaluationError> ParseFiniteNumber(std::string_view text);
std::expected<double, EvaluationError> EvaluateChecked(std::string_view expression, double x);
} // namespace fastecu::expression
