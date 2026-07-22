#ifndef EXPRESSION_EVALUATOR_H
#define EXPRESSION_EVALUATOR_H

#include <string>
#include <string_view>
#include <vector>

std::vector<std::string> expression_parse(std::string_view expression, std::string_view x);
double expression_evaluate(std::vector<std::string> expression, int precision = 15);
double expression_evaluate(std::string_view expression, std::string_view x, int precision = 15);

#endif // EXPRESSION_EVALUATOR_H
