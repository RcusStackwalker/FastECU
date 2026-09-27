#include "src/backend/definition/parser_utils.h"

#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{

TEST(ParserUtilsTest, AdoptInlineScalingTakesTheNameAndFillsOnlyUnsetStorageAndEndian)
{
    UnresolvedScaling scaling;
    scaling.name = "inline";
    scaling.storage_type = StorageType::Uint8;
    scaling.endian = "little";

    UnresolvedCalibrationMap unset;
    adopt_inline_scaling(scaling, unset);
    EXPECT_EQ(unset.scaling_name, "inline");
    EXPECT_EQ(unset.storage_type, StorageType::Uint8);
    EXPECT_EQ(unset.endian, "little");

    UnresolvedCalibrationMap set;
    set.scaling_name = "reference";
    set.storage_type = StorageType::Uint16;
    set.endian = "big";
    adopt_inline_scaling(scaling, set);
    EXPECT_EQ(set.scaling_name, "inline");
    EXPECT_EQ(set.storage_type, StorageType::Uint16);
    EXPECT_EQ(set.endian, "big");
}

TEST(ParserUtilsTest, MapScalingFallbackNamePrefersReferenceThenIdThenName)
{
    UnresolvedCalibrationMap map;
    map.name = "Fuel";
    EXPECT_EQ(map_scaling_fallback_name(map), "Fuel");
    map.id = "fuel-primary";
    EXPECT_EQ(map_scaling_fallback_name(map), "fuel-primary");
    map.scaling_name = "fuel-scale";
    EXPECT_EQ(map_scaling_fallback_name(map), "fuel-scale");
}

} // namespace
} // namespace fastecu::definition
