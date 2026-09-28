#include "src/ui/desktop/calibration/map_presentation.h"

#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{
calibration::CalibrationSession session(std::string type = "3D")
{
    definition::RomDefinition definition{.format = definition::DefinitionFormat::EcuFlash};
    definition.scalings.push_back({.name = "raw", .units = "rpm", .format = "0.00"});
    definition::CalibrationMap map;
    map.name = "Fuel";
    map.type = type;
    map.x_size = 2;
    map.y_size = 2;
    map.address = 0;
    map.storage_type = definition::StorageType::Uint8;
    map.scaling_name = "raw";
    definition.maps.push_back(map);
    return calibration::CalibrationSession(calibration::SessionId{1},
                                           {
                                               .rom = {1, 2, 3, 4},
                                               .definition = calibration::ResolvedDefinition{.definition = definition},
                                           });
}

TEST(MapPresentation, FormatsDecodedCellsAndAbsentAxes)
{
    const auto rom = session();
    const auto shown = present_map(rom, 0);
    ASSERT_TRUE(shown.has_value());
    EXPECT_EQ(shown->body, (QStringList{"1", "2", "3", "4", ""}));
    EXPECT_EQ(shown->x_axis, (QStringList{" "}));
    EXPECT_EQ(shown->name, "Fuel");
    EXPECT_EQ(shown->x_name, " ");
    EXPECT_EQ(shown->units, "rpm");
    EXPECT_EQ(format_map_value(shown->body[0], shown->format), "1.00");
}

TEST(MapPresentation, ColorBoundsStayAtOpeningValuesAfterEdit)
{
    auto rom = session();
    auto shown = present_map(rom, 0);
    ASSERT_TRUE(shown.has_value());
    const auto bounds = opening_color_bounds(*shown);
    EXPECT_EQ(bounds.minimum, 1.0F);
    EXPECT_EQ(bounds.maximum, 4.0F);
    const bytes::Bytes change{20};
    ASSERT_TRUE(rom.write_bytes(0, change).has_value());
    shown = present_map(rom, 0);
    ASSERT_TRUE(shown.has_value());
    EXPECT_EQ(shown->body[0], "20");
    EXPECT_EQ(map_cell_color(20, bounds), map_cell_color(4, bounds));
    EXPECT_EQ(opening_color_bounds(*shown).maximum, 20.0F);
}

TEST(MapPresentation, ConstantRangeHasValidColor)
{
    EXPECT_EQ(map_cell_color(3, {3, 3}), QColor::fromHsvF(0, 0.85, 0.85));
}
} // namespace
} // namespace fastecu::ui
