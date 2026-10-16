#include "src/algorithms/expression/expression.h"

#include <cmath>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace fastecu::expression
{
namespace
{
// Any conversion expression text compiles or is rejected without a fault; a
// compiled expression yields only finite values or a reported error, and the
// one-shot evaluation agrees with compiling first.
void EvaluatesAnyExpressionSafely(const std::string& text, double x)
{
    const auto compiled = Compile(text);
    const auto checked = EvaluateChecked(text, x);
    if (!compiled.has_value())
    {
        EXPECT_FALSE(checked.has_value());
        EXPECT_FALSE(compiled.error().detail.empty());
        return;
    }

    const auto value = compiled->Evaluate(x);
    ASSERT_EQ(value.has_value(), checked.has_value());
    if (value.has_value())
    {
        EXPECT_TRUE(std::isfinite(*value));
        EXPECT_EQ(*value, *checked);
    }
    else
    {
        EXPECT_FALSE(value.error().detail.empty());
    }
}
FUZZ_TEST(ExpressionFuzz, EvaluatesAnyExpressionSafely)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<double>())
    .WithSeeds(
        []
        {
            std::vector<std::tuple<std::string, double>> seeds;
            for (auto& [text] : fuzztest::ReadFilesFromDirectory("src/algorithms/expression/testdata/fuzz/expressions"))
            {
                seeds.emplace_back(std::move(text), 2.0);
            }
            return seeds;
        });

// A parsed number is always finite.
void ParsesOnlyFiniteNumbers(const std::string& text)
{
    const auto number = ParseFiniteNumber(text);
    if (number.has_value())
    {
        EXPECT_TRUE(std::isfinite(*number));
    }
}
FUZZ_TEST(ExpressionFuzz, ParsesOnlyFiniteNumbers);
} // namespace
} // namespace fastecu::expression
