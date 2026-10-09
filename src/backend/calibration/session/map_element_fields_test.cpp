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

definition::RomDefinition two_by_two_definition()
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
    map.address = 16;
    map.storage_type = definition::StorageType::kUint16;
    map.endian = "big";
    map.scaling_name = "body";
    map.x_size = 2;
    map.y_size = 2;
    map.start_position = 2;
    map.interval = 3;
    map.x_axis.type = "X Axis";
    map.x_axis.address = 64;
    map.x_axis.storage_type = definition::StorageType::kUint8;
    map.x_axis.from_byte = "x*10";
    map.x_axis.to_byte = "x/10";
    map.y_axis.type = "Y Axis";
    map.y_axis.address = 80;
    map.y_axis.storage_type = definition::StorageType::kInt16;
    map.y_axis.endian = "big";
    map.y_axis.from_byte = "x/4";
    map.y_axis.to_byte = "x*4";
    def.maps.push_back(map);
    return def;
}

CalibrationSession session_from(definition::RomDefinition def = two_by_two_definition())
{
    SessionContents contents;
    contents.rom.resize(128);
    contents.definition = ResolvedDefinition{.definition = std::move(def)};
    contents.protocol.flash_method = "wrx02";
    contents.protocol.unpadded_size = 123;
    return CalibrationSession(SessionId{1}, std::move(contents));
}

TEST(MapElementFields, PlucksTypedFieldsAndUnpaddedProtocolSize)
{
    const auto session = session_from();
    const auto fields = collect_map_element_fields(session, 0, NumericTarget::kMapBody);
    const auto spec = fields.spec();
    EXPECT_EQ(spec.address, 16U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::kUint16);
    EXPECT_EQ(spec.from_byte, "x*2");
    EXPECT_EQ(spec.to_byte, "x");
    EXPECT_EQ(spec.min_value, "");
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.1);
    EXPECT_EQ(spec.start_position, 2U);
    EXPECT_EQ(spec.interval, 3U);
    EXPECT_EQ(spec.flash_method, "wrx02");
    EXPECT_EQ(spec.rom_file_size, 123U);
}

TEST(MapElementFields, AxisUsesResolvedFieldsAndScalingBounds)
{
    auto def = two_by_two_definition();
    def.maps[0].x_axis.scaling_name = "body";
    def.maps[0].x_axis.start_position = 4;
    def.maps[0].x_axis.interval = 5;
    const auto session = session_from(std::move(def));
    const auto fields = collect_map_element_fields(session, 0, NumericTarget::kXAxis);
    const auto spec = fields.spec();
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
    auto def = two_by_two_definition();
    def.maps[0].storage_type.reset();
    def.maps[0].endian.clear();
    def.scalings[0].storage_type = definition::StorageType::kFloat;
    def.scalings[0].endian = "little";
    def.maps[0].x_axis.scaling_name = "body";
    const auto session = session_from(std::move(def));
    const auto body = collect_map_element_fields(session, 0, NumericTarget::kMapBody);
    EXPECT_EQ(body.spec().storage_type, definition::StorageType::kFloat);
    EXPECT_EQ(body.spec().endian, "little");
    const auto axis = collect_map_element_fields(session, 0, NumericTarget::kXAxis);
    EXPECT_EQ(axis.spec().storage_type, definition::StorageType::kUint8);
    EXPECT_EQ(axis.spec().endian, "");
}

TEST(MapElementFields, MissingScalingAndAxisYieldEmptyFields)
{
    auto def = two_by_two_definition();
    def.maps[0].scaling_name.clear();
    def.maps[0].x_axis = {};
    const auto session = session_from(std::move(def));
    const auto body = collect_map_element_fields(session, 0, NumericTarget::kMapBody);
    EXPECT_EQ(body.spec().from_byte, "x");
    const auto axis = collect_map_element_fields(session, 0, NumericTarget::kXAxis);
    EXPECT_EQ(axis.spec().endian, "");
    EXPECT_EQ(axis.spec().to_byte, "");
    EXPECT_EQ(axis.spec().address, 0U);
    EXPECT_EQ(axis.spec().start_position, 1U);
}

TEST(MapElementFields, YAxisUsesItsOwnAxisDefinition)
{
    const auto session = session_from();
    const auto fields = collect_map_element_fields(session, 0, NumericTarget::kYAxis);
    const auto spec = fields.spec();
    EXPECT_EQ(spec.address, 80U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::kInt16);
    EXPECT_EQ(spec.endian, "big");
    EXPECT_EQ(spec.to_byte, "x*4");
    EXPECT_EQ(spec.x_size, 2U);
    EXPECT_EQ(spec.y_size, 2U);
}

} // namespace
} // namespace fastecu::calibration
