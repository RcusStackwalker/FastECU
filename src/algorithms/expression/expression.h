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

std::expected<double, EvaluationError> ParseFiniteNumber(std::string_view text);
std::expected<double, EvaluationError> EvaluateChecked(std::string_view expression, double x);
} // namespace fastecu::expression
