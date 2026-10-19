#include "src/backend/logging/logger_definition_parser.h"

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace fastecu::logging
{
namespace
{
bytes::ByteView View(const std::string& text)
{
    return {reinterpret_cast<const bytes::Byte *>(text.data()), text.size()};
}

// Any document parses or is rejected with a reported error, and parsing is
// deterministic.
void ParsesAnyLoggerDefinitionSafely(const std::string& xml)
{
    const auto parsed = ParseLoggerDefinition(View(xml), "fuzz.xml");
    if (!parsed.has_value())
    {
        EXPECT_FALSE(parsed.error().detail.empty());
        return;
    }

    const auto again = ParseLoggerDefinition(View(xml), "fuzz.xml");
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*parsed, *again);
}
FUZZ_TEST(LoggerDefinitionParserFuzz, ParsesAnyLoggerDefinitionSafely)
    .WithDomains(fuzztest::Arbitrary<std::string>())
    .WithSeeds(
        []
        {
            std::vector<std::tuple<std::string>> seeds;
            for (auto& seed : fuzztest::ReadFilesFromDirectory("src/backend/logging/testdata/fuzz/logger_definition"))
            {
                seeds.push_back(std::move(seed));
            }
            return seeds;
        });
} // namespace
} // namespace fastecu::logging
