#include "src/backend/definition/definition_writer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

#include "src/backend/definition/ecuflash_parser.h"

namespace fastecu::definition
{
namespace
{
// Valid XML text: markup characters, tab, newline and multi-byte UTF-8. NUL and
// CR are excluded because the writer does not preserve them (docs/tech-debt.md).
const std::vector<std::string>& TextPieces()
{
    static const std::vector<std::string> pieces{"a", "Z",  "7", " ",  "-",  "_",        "<",       ">",
                                                 "&", "\"", "'", "\t", "\n", "\xC3\xA9", "\xCE\xA9"};
    return pieces;
}

std::string JoinPieces(const std::vector<std::string>& pieces)
{
    std::string text;
    for (const auto& piece : pieces)
    {
        text += piece;
    }
    return text;
}

// Splits text back into pieces so seeds can name a value; nullopt when the
// text is not made of pieces.
std::optional<std::tuple<std::vector<std::string>>> SplitPieces(const std::string& text)
{
    std::vector<std::string> pieces;
    for (std::size_t offset = 0; offset < text.size();)
    {
        const auto match = std::ranges::find_if(TextPieces(), [&](const std::string& piece)
                                                { return text.compare(offset, piece.size(), piece) == 0; });
        if (match == TextPieces().end())
        {
            return std::nullopt;
        }
        pieces.push_back(*match);
        offset += match->size();
    }
    return std::tuple{std::move(pieces)};
}

auto Texts()
{
    return fuzztest::ReversibleMap(JoinPieces, SplitPieces,
                                   fuzztest::VectorOf(fuzztest::ElementOf(TextPieces())).WithMaxSize(12));
}

auto Addresses()
{
    return fuzztest::OptionalOf(fuzztest::ReversibleMap(
        [](std::uint32_t value) { return memory::DefinitionAddress{value}; }, [](memory::DefinitionAddress address)
        { return std::optional{std::tuple{address.Value()}}; }, fuzztest::Arbitrary<std::uint32_t>()));
}

auto HeaderInputs()
{
    return fuzztest::StructOf<DefinitionHeaderInput>(Texts(), Texts(), Texts(), Addresses(),
                                                     fuzztest::StructOf<RomMetadata>(Texts(), Texts(), Texts(), Texts(),
                                                                                     Texts(), Texts(), Texts(), Texts(),
                                                                                     Texts(), Texts(), Texts()),
                                                     Texts(), Texts());
}

// What the parser reads back from written XML is the normalized input.
void ExpectReadsBack(const std::vector<std::uint8_t>& xml, const DefinitionHeaderInput& input)
{
    const auto expected = NormalizeHeaderInput(input);
    const auto parsed = ParseEcuflashDefinition(xml, "written.xml");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().detail;
    EXPECT_EQ(parsed->identity.xml_id, expected.xml_id);
    EXPECT_EQ(parsed->identity.internal_id, expected.internal_id);
    EXPECT_EQ(parsed->identity.ecu_id, expected.ecu_id);
    EXPECT_EQ(parsed->identity.internal_id_address, expected.internal_id_address);
    EXPECT_EQ(parsed->metadata, expected.metadata);
}

// Writing a definition header either fails with a reported error or yields
// XML whose header reads back as the normalized input.
void CreatedXmlReadsBack(const DefinitionHeaderInput& input)
{
    const auto created = CreateEcuflashXml(input);
    if (!created.has_value())
    {
        EXPECT_FALSE(created.error().detail.empty());
        return;
    }
    ExpectReadsBack(*created, input);
}
FUZZ_TEST(DefinitionWriterFuzz, CreatedXmlReadsBack).WithDomains(HeaderInputs());

// Rewriting any document either fails with a reported error or yields XML
// whose header reads back as the normalized input.
void RewrittenXmlReadsBack(const std::vector<std::uint8_t>& source, const DefinitionHeaderInput& input)
{
    const auto rewritten = RewriteEcuflashXml(source, input);
    if (!rewritten.has_value())
    {
        EXPECT_FALSE(rewritten.error().detail.empty());
        return;
    }
    ExpectReadsBack(*rewritten, input);
}
FUZZ_TEST(DefinitionWriterFuzz, RewrittenXmlReadsBack)
    .WithDomains(fuzztest::Arbitrary<std::vector<std::uint8_t>>(), HeaderInputs())
    .WithSeeds(
        []
        {
            DefinitionHeaderInput input{.xml_id = "aZ_7", .internal_id = "7a7a", .ecu_id = "Za-7"};
            std::vector<std::tuple<std::vector<std::uint8_t>, DefinitionHeaderInput>> seeds;
            for (auto& [text] : fuzztest::ReadFilesFromDirectory("src/backend/definition/testdata/fuzz/writer"))
            {
                seeds.emplace_back(std::vector<std::uint8_t>(text.begin(), text.end()), input);
            }
            return seeds;
        });
} // namespace
} // namespace fastecu::definition
