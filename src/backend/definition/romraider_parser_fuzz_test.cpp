#include "src/backend/definition/romraider_parser.h"

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{
constexpr auto kSeedDirectory = "src/backend/definition/testdata/fuzz/romraider";

// Any bytes parse or are rejected with a reported error; an index entry
// carries the source and format it was parsed under; parsing is deterministic.
void IndexesAnyBytesSafely(const std::vector<std::uint8_t>& xml)
{
    const auto index = ParseRomraiderIndex(xml, "fuzz.xml");
    if (!index.has_value())
    {
        EXPECT_FALSE(index.error().detail.empty());
        return;
    }

    for (const auto& entry : *index)
    {
        EXPECT_EQ(entry.format, DefinitionFormat::kRomRaider);
        EXPECT_EQ(entry.source, "fuzz.xml");
    }
    const auto again = ParseRomraiderIndex(xml, "fuzz.xml");
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*index, *again);
}
FUZZ_TEST(RomRaiderParserFuzz, IndexesAnyBytesSafely)
    .WithDomains(fuzztest::Arbitrary<std::vector<std::uint8_t>>())
    .WithSeeds(
        []
        {
            std::vector<std::tuple<std::vector<std::uint8_t>>> seeds;
            for (auto& [text] : fuzztest::ReadFilesFromDirectory(kSeedDirectory))
            {
                seeds.emplace_back(std::vector<std::uint8_t>(text.begin(), text.end()));
            }
            return seeds;
        });

// Any bytes parse into a definition or are rejected with a reported error;
// a parsed definition carries the source and format it was parsed under, and
// parsing is deterministic.
void ParsesAnyDefinitionSafely(const std::vector<std::uint8_t>& xml, const std::string& definition_id)
{
    const auto parse = [&] { return ParseRomraiderDefinition(xml, "fuzz.xml", definition_id); };
    const auto definition = parse();
    if (!definition.has_value())
    {
        EXPECT_FALSE(definition.error().detail.empty());
        return;
    }

    EXPECT_EQ(definition->format, DefinitionFormat::kRomRaider);
    EXPECT_EQ(definition->source, "fuzz.xml");
    const auto again = parse();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*definition, *again);
}
FUZZ_TEST(RomRaiderParserFuzz, ParsesAnyDefinitionSafely)
    .WithDomains(fuzztest::Arbitrary<std::vector<std::uint8_t>>(), fuzztest::Arbitrary<std::string>())
    .WithSeeds(
        []
        {
            std::vector<std::tuple<std::vector<std::uint8_t>, std::string>> seeds;
            for (auto& [text] : fuzztest::ReadFilesFromDirectory(kSeedDirectory))
            {
                seeds.emplace_back(std::vector<std::uint8_t>(text.begin(), text.end()), "CHILD");
            }
            return seeds;
        });
} // namespace
} // namespace fastecu::definition
