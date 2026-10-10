#include "src/ui/desktop/calibration/map_presentation.h"
#include "src/algorithms/memory/testing/memory_views.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{
calibration::CalibrationSession session(std::string_view expression = "x")
{
    definition::RomDefinition definition{.format = definition::DefinitionFormat::kEcuFlash};
    definition.scalings.push_back(
        {.name = "raw", .units = "rpm", .from_byte = std::string(expression), .format = "0.00"});
    definition::CalibrationMap map;
    map.name = "Fuel";
    map.type = "3D";
    map.x_size = 2;
    map.y_size = 2;
    map.address = memory::DefinitionAddress{0};
    map.storage_type = definition::StorageType::kUint8;
    map.scaling_name = "raw";
    definition.maps.push_back(map);
    return calibration::CalibrationSession(calibration::SessionId{1},
                                           {.image = memory::testing::IdentityImage({1, 2, 3, 4}),
                                            .definition = calibration::ResolvedDefinition{.definition = definition}});
}

TEST(MapPresentation, FormatsNumericCellsAndExplicitAbsentAxes)
{
    const auto rom = session();
    const auto shown = presentMap(rom, 0);
    ASSERT_THAT(shown, fastecu::testing::IsOk());
    EXPECT_THAT(shown->body, ::testing::ElementsAre(::testing::Field(&PresentedCell::text, QString("1.00")),
                                                    ::testing::Field(&PresentedCell::text, QString("2.00")),
                                                    ::testing::Field(&PresentedCell::text, QString("3.00")),
                                                    ::testing::Field(&PresentedCell::text, QString("4.00"))));
    EXPECT_FALSE(shown->x_axis.has_value());
    EXPECT_FALSE(shown->y_axis.has_value());
    EXPECT_EQ(shown->name, "Fuel");
    EXPECT_EQ(shown->units, "rpm");
}

TEST(MapPresentation, KeepsFullNumericValueBehindFormattedText)
{
    const auto rom = session("x/10000");
    const auto shown = presentMap(rom, 0);
    ASSERT_THAT(shown, fastecu::testing::IsOk());
    EXPECT_EQ(shown->body[0].text, "0.00");
    EXPECT_EQ(shown->body[0].numeric_value, 0.0001);
}

TEST(MapPresentation, ColorBoundsStayAtOpeningValuesAfterEdit)
{
    auto rom = session();
    auto shown = presentMap(rom, 0);
    ASSERT_THAT(shown, fastecu::testing::IsOk());
    const auto bounds = openingColorBounds(*shown);
    ASSERT_TRUE(bounds.has_value());
    EXPECT_EQ(bounds->minimum, 1.0);
    EXPECT_EQ(bounds->maximum, 4.0);
    ASSERT_THAT(rom.WriteBytes(0, bytes::Bytes{20}), fastecu::testing::IsOk());
    shown = presentMap(rom, 0);
    ASSERT_THAT(shown, fastecu::testing::IsOk());
    EXPECT_EQ(shown->body[0].text, "20.00");
    EXPECT_EQ(mapCellColor(20, *bounds), mapCellColor(4, *bounds));
    const auto newBounds = openingColorBounds(*shown);
    ASSERT_TRUE(newBounds.has_value());
    EXPECT_EQ(newBounds->maximum, 20.0);
}

TEST(MapPresentation, ExcludesInvalidCellsFromColorBounds)
{
    const auto rom = session("1/(x-1)");
    const auto shown = presentMap(rom, 0);
    ASSERT_THAT(shown, fastecu::testing::IsOk());
    EXPECT_EQ(shown->body[0].text, "NaN");
    EXPECT_FALSE(shown->body[0].diagnostic.isEmpty());
    EXPECT_FALSE(shown->body[0].numeric_value.has_value());
    const auto bounds = openingColorBounds(*shown);
    ASSERT_TRUE(bounds.has_value());
    EXPECT_DOUBLE_EQ(bounds->minimum, 1.0 / 3.0);
    EXPECT_DOUBLE_EQ(bounds->maximum, 1.0);
}

TEST(MapPresentation, AllInvalidCellsHaveNoNumericColorBounds)
{
    const auto rom = session("1/0");
    const auto shown = presentMap(rom, 0);
    ASSERT_THAT(shown, fastecu::testing::IsOk());
    EXPECT_FALSE(openingColorBounds(*shown).has_value());
}

TEST(MapPresentation, ColorsFiniteValuesAcrossExtremeBounds)
{
    const MapColorBounds bounds{-1e308, 1e308};
    EXPECT_EQ(mapCellColor(-1e308, bounds), QColor::fromHsvF(0, 0.85F, 0.85F));
    EXPECT_EQ(mapCellColor(0, bounds), QColor::fromHsvF(0.29166667F, 0.85F, 0.85F));
    EXPECT_EQ(mapCellColor(1e308, bounds), QColor::fromHsvF(0.5833333F, 0.85F, 0.85F));
}

TEST(MapPresentation, ConstantRangeHasValidColor)
{
    EXPECT_EQ(mapCellColor(3, {3, 3}), QColor::fromHsvF(0, 0.85, 0.85));
}

TEST(MapPresentation, NumericSelectionIndexRequiresRepresentableInteger)
{
    EXPECT_EQ(selectionIndex(PresentedCell{.numeric_value = 12.0}), 12);
    EXPECT_EQ(selectionIndex(PresentedCell{.numeric_value = -1.0}), -1);
    EXPECT_EQ(selectionIndex(PresentedCell{.numeric_value = 2.5}), 0);
    EXPECT_EQ(selectionIndex(PresentedCell{.numeric_value = 1e100}), 0);
    EXPECT_EQ(selectionIndex(PresentedCell{.text = "NaN", .diagnostic = "invalid"}), 0);
}
} // namespace
} // namespace fastecu::ui
