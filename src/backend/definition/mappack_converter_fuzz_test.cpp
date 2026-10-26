#include "src/backend/definition/mappack_converter.h"

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

#include "src/backend/definition/romraider_parser.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::definition
{
namespace
{
// Any CSV converts or is rejected as invalid configuration with a reported
// error; converted output is a RomRaider document its own parser accepts.
void ConvertsAnyCsvSafely(const std::string& csv, const std::string& ecu_id)
{
    const auto converted = ConvertMappackCsv(csv, ecu_id);
    if (!converted.has_value())
    {
        EXPECT_THAT(converted, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
        EXPECT_FALSE(converted.error().detail.empty());
        return;
    }

    const std::vector<std::uint8_t> xml(converted->begin(), converted->end());
    EXPECT_TRUE(ParseRomraiderIndex(xml, "mappack.xml").has_value());
}
FUZZ_TEST(MappackConverterFuzz, ConvertsAnyCsvSafely)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>())
    .WithSeeds(
        []
        {
            std::vector<std::tuple<std::string, std::string>> seeds;
            for (auto& [csv] : fuzztest::ReadFilesFromDirectory("src/backend/definition/testdata/fuzz/mappack"))
            {
                seeds.emplace_back(csv, "ECU123");
                // CSV-only corpus files also exercise the empty ECU-ID boundary.
                seeds.emplace_back(std::move(csv), "");
            }
            return seeds;
        });
} // namespace
} // namespace fastecu::definition
