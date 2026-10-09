#pragma once

#include <string>
#include <string_view>
#include <vector>

std::vector<std::string> ExpressionParse(std::string_view expression, std::string_view x);
double ExpressionEvaluate(std::vector<std::string> expression, int precision = 15);
double ExpressionEvaluate(std::string_view expression, std::string_view x, int precision = 15);
