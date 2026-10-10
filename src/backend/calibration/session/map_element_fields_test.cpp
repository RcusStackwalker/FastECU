#include "src/backend/calibration/session/map_element_fields.h"

#include <cstdint>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::calibration
{
namespace
{

// MapElementFields::spec() is ref-qualified (`const &`, with `const && =
// delete`) so that `collect_map_element_fields(...).spec()` -- taking a spec
// from a temporary that is gone by the semicolon -- is a compile error
// instead of a dangle. Enforcement lives entirely in the ref-qualification;
// a requires-expression cannot observe a deleted overload, so no
// static_assert pins it here.

definition::RomDefinition TwoByTwoDefinition()
{
    definition::RomDefinition def;
    definition::Scaling scaling;
    scaling.name = "body";
    scaling.from_byte = "x*2";
    scaling.coarse_increment = "1.0";
    scaling.fine_increment = "0.1";
    def.scalings.push_back(scaling);
    definition::CalibrationMap map;
    map.name = "Timing";
    map.address = memory::DefinitionAddress{16};
    map.storage_type = definition::StorageType::kUint16;
    map.endian = "big";
    map.scaling_name = "body";
    map.x_size = 2;
    map.y_size = 2;
    map.start_position = 2;
    map.interval = 3;
    map.x_axis.type = "X Axis";
    map.x_axis.address = memory::DefinitionAddress{64};
    map.x_axis.storage_type = definition::StorageType::kUint8;
    map.x_axis.from_byte = "x*10";
    map.x_axis.to_byte = "x/10";
    map.y_axis.type = "Y Axis";
    map.y_axis.address = memory::DefinitionAddress{80};
    map.y_axis.storage_type = definition::StorageType::kInt16;
    map.y_axis.endian = "big";
    map.y_axis.from_byte = "x/4";
    map.y_axis.to_byte = "x*4";
    def.maps.push_back(map);
    return def;
}

CalibrationSession SessionFrom(definition::RomDefinition def = TwoByTwoDefinition())
{
    SessionContents contents;
    contents.rom.resize(128);
    contents.definition = ResolvedDefinition{.definition = std::move(def)};
    return CalibrationSession(SessionId{1}, std::move(contents));
}

TEST(MapElementFields, PlucksTypedFields)
{
    const auto session = SessionFrom();
    const auto fields = CollectMapElementFields(session, 0, NumericTarget::kMapBody);
    const auto spec = fields.Spec();
    EXPECT_EQ(spec.address, 16U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::kUint16);
    EXPECT_EQ(spec.from_byte, "x*2");
    EXPECT_EQ(spec.to_byte, "x");
    EXPECT_EQ(spec.min_value, "");
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.1);
    EXPECT_EQ(spec.start_position, 2U);
    EXPECT_EQ(spec.interval, 3U);
}

TEST(MapElementFields, AxisUsesResolvedFieldsAndScalingBounds)
{
    auto def = TwoByTwoDefinition();
    def.maps[0].x_axis.scaling_name = "body";
    def.maps[0].x_axis.start_position = 4;
    def.maps[0].x_axis.interval = 5;
    const auto session = SessionFrom(std::move(def));
    const auto fields = CollectMapElementFields(session, 0, NumericTarget::kXAxis);
    const auto spec = fields.Spec();
    EXPECT_EQ(spec.address, 64U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::kUint8);
    EXPECT_EQ(spec.from_byte, "x*10");
    EXPECT_EQ(spec.to_byte, "x/10");
    EXPECT_EQ(spec.start_position, 4U);
    EXPECT_EQ(spec.interval, 5U);
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.1);
}

TEST(MapElementFields, BodyStorageAndEndianFallBackToScalingButAxesUseResolvedStorage)
{
    auto def = TwoByTwoDefinition();
    def.maps[0].storage_type.reset();
    def.maps[0].endian.clear();
    def.scalings[0].storage_type = definition::StorageType::kFloat;
    def.scalings[0].endian = "little";
    def.maps[0].x_axis.scaling_name = "body";
    const auto session = SessionFrom(std::move(def));
    const auto body = CollectMapElementFields(session, 0, NumericTarget::kMapBody);
    EXPECT_EQ(body.Spec().storage_type, definition::StorageType::kFloat);
    EXPECT_EQ(body.Spec().endian, "little");
    const auto axis = CollectMapElementFields(session, 0, NumericTarget::kXAxis);
    EXPECT_EQ(axis.Spec().storage_type, definition::StorageType::kUint8);
    EXPECT_EQ(axis.Spec().endian, "");
}

TEST(MapElementFields, MissingScalingAndAxisYieldEmptyFields)
{
    auto def = TwoByTwoDefinition();
    def.maps[0].scaling_name.clear();
    def.maps[0].x_axis = {};
    const auto session = SessionFrom(std::move(def));
    const auto body = CollectMapElementFields(session, 0, NumericTarget::kMapBody);
    EXPECT_EQ(body.Spec().from_byte, "x");
    const auto axis = CollectMapElementFields(session, 0, NumericTarget::kXAxis);
    EXPECT_EQ(axis.Spec().endian, "");
    EXPECT_EQ(axis.Spec().to_byte, "");
    EXPECT_EQ(axis.Spec().address, 0U);
    EXPECT_EQ(axis.Spec().start_position, 1U);
}

TEST(MapElementFields, YAxisUsesItsOwnAxisDefinition)
{
    const auto session = SessionFrom();
    const auto fields = CollectMapElementFields(session, 0, NumericTarget::kYAxis);
    const auto spec = fields.Spec();
    EXPECT_EQ(spec.address, 80U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::kInt16);
    EXPECT_EQ(spec.endian, "big");
    EXPECT_EQ(spec.to_byte, "x*4");
    EXPECT_EQ(spec.x_size, 2U);
    EXPECT_EQ(spec.y_size, 2U);
}

} // namespace
} // namespace fastecu::calibration
