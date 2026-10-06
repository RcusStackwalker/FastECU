#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace fastecu::expression
{
struct EvaluationError
{
    std::string detail;
};

std::expected<double, EvaluationError> parse_finite_number(std::string_view text);
std::expected<double, EvaluationError> evaluate_checked(std::string_view expression, double x);
} // namespace fastecu::expression
