#include "src/backend/calibration/definition_interpretation_internal.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/calibration_service.h"
#include "src/backend/calibration/definition_bytes.h"
#include "src/backend/calibration/numeric_map_edit.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::VariantWith;

template <typename T>
concept HasRvalueEditSpec = requires(T value) { std::move(value).EditSpec(); };

static_assert(!HasRvalueEditSpec<internal::ResolvedElement>);

class DefinitionInterpretation : public ::testing::TestWithParam<std::size_t>
{
};

TEST_P(DefinitionInterpretation, ResolvedElementOwnsMetadataAcrossMoves)
{
    internal::ResolvedElement moved;
    {
        definition::RomDefinition definition;
        definition::Scaling scaling;
        scaling.name = "body";
        scaling.storage_type = definition::StorageType::kUint16;
        scaling.endian = "little";
        scaling.from_byte = "x*2" + std::string(GetParam(), ' ');
        scaling.to_byte = "x/2" + std::string(GetParam(), ' ');
        scaling.minimum = "0";
        scaling.maximum = "9000";
        scaling.coarse_increment = "60";
        scaling.fine_increment = "1";
        definition.scalings.push_back(scaling);
        definition::CalibrationMap map;
        map.address = memory::DefinitionAddress{0};
        map.x_size = 2;
        map.scaling_name = "body";
        auto original = internal::ResolveMapElement(definition, map, NumericTarget::kMapBody);
        moved = std::move(original);
    }
    const ElementRun run{.address = moved.address->Value(),
                         .count = static_cast<std::uint32_t>(moved.count),
                         .start_position = moved.start_position,
                         .interval = moved.interval,
                         .storage_type = moved.storage_type,
                         .endian = moved.endian,
                         .from_byte = moved.from_byte,
                         .is_selectable = moved.is_selectable};
    const bytes::Bytes rom{0xE8, 0x03, 0xD0, 0x07};
    const auto decoded = DecodeNumericRun(DefinitionBytes(rom), run);
    ASSERT_THAT(decoded, IsOk());
    EXPECT_THAT(decoded->cells, ElementsAre(IsOkAnd(2000), IsOkAnd(4000)));
    const auto spec = moved.EditSpec();
    EXPECT_DOUBLE_EQ(spec.coarse_increment, 60);
    EXPECT_DOUBLE_EQ(spec.fine_increment, 1);
    EXPECT_EQ(spec.min_value, "0");
    EXPECT_EQ(spec.max_value, "9000");
    const auto encoded = CalculateAssignment(DefinitionBytes(rom), spec, 2, decoded->cells,
                                             {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, "7500");
    ASSERT_THAT(encoded, IsOk());
    ASSERT_EQ(encoded->writes.size(), 1U);
    EXPECT_THAT(encoded->writes[0].bytes, ElementsAre(0xA6, 0x0E));
}

INSTANTIATE_TEST_SUITE_P(ExpressionStorage, DefinitionInterpretation, ::testing::Values(0U, 128U));

TEST(ResolveMapElement, UsesResolvedAxisFieldsAndScalingBounds)
{
    definition::RomDefinition definition;
    definition::Scaling scaling;
    scaling.name = "axis";
    scaling.storage_type = definition::StorageType::kUint16;
    scaling.endian = "little";
    scaling.from_byte = "x*3";
    scaling.to_byte = "x/3";
    scaling.minimum = "-10";
    scaling.maximum = "100";
    scaling.coarse_increment = "  ";
    scaling.fine_increment = "invalid";
    definition.scalings.push_back(scaling);
    definition::CalibrationMap map;
    map.x_size = 3;
    map.y_size = 4;
    map.x_axis.address = memory::DefinitionAddress{12};
    map.x_axis.scaling_name = "axis";
    map.x_axis.from_byte = "x*2";
    map.x_axis.to_byte = "x/2";
    map.x_axis.start_position = 2;
    map.x_axis.interval = 3;
    const auto resolved = internal::ResolveMapElement(definition, map, NumericTarget::kXAxis);
    EXPECT_EQ(resolved.address, memory::DefinitionAddress{12});
    EXPECT_EQ(resolved.storage_type, std::nullopt);
    EXPECT_TRUE(resolved.endian.empty());
    EXPECT_EQ(resolved.from_byte, "x*2");
    EXPECT_EQ(resolved.to_byte, "x/2");
    EXPECT_EQ(resolved.min_value, "-10");
    EXPECT_EQ(resolved.max_value, "100");
    EXPECT_DOUBLE_EQ(resolved.coarse_increment, 0);
    EXPECT_TRUE(std::isnan(resolved.fine_increment));
    EXPECT_EQ(resolved.count, 3U);
    EXPECT_EQ(resolved.width, 3U);
    EXPECT_EQ(resolved.height, 1U);
    EXPECT_EQ(resolved.start_position, 2U);
    EXPECT_EQ(resolved.interval, 3U);
    EXPECT_EQ(resolved.EditSpec().x_size, 3U);
    EXPECT_EQ(resolved.EditSpec().y_size, 4U);
    const auto y = internal::ResolveMapElement(definition, map, NumericTarget::kYAxis);
    EXPECT_EQ(y.address, std::nullopt);
    EXPECT_EQ(y.count, 4U);
    EXPECT_EQ(y.width, 1U);
    EXPECT_EQ(y.height, 4U);
}

TEST(ResolveMapElement, KeepsWideBodyCountAndMissingAddress)
{
    definition::CalibrationMap map;
    map.x_size = std::numeric_limits<std::uint32_t>::max();
    map.y_size = 2;
    map.type = "Selectable";
    const auto resolved = internal::ResolveMapElement({}, map, NumericTarget::kMapBody);
    EXPECT_EQ(resolved.address, std::nullopt);
    EXPECT_EQ(resolved.count, 8589934590ULL);
    EXPECT_EQ(resolved.from_byte, "x");
    EXPECT_EQ(resolved.to_byte, "x");
    EXPECT_TRUE(resolved.is_selectable);
}

TEST(ElementByteWidthTest, DelegatesToStorageByteSizeWhenNotBloblist)
{
    EXPECT_EQ(internal::ElementByteWidth(definition::StorageType::kUint16, nullptr), 2U);
    EXPECT_EQ(internal::ElementByteWidth(std::optional<definition::StorageType>{}, nullptr), 1U);
}

TEST(ElementByteWidthTest, BloblistWithNoScalingFallsBackToOneByte)
{
    EXPECT_EQ(internal::ElementByteWidth(definition::StorageType::kBloblist, nullptr), 1U);
}

TEST(ElementByteWidthTest, BloblistWithEmptySelectionsFallsBackToOneByte)
{
    definition::Scaling scaling;
    EXPECT_EQ(internal::ElementByteWidth(definition::StorageType::kBloblist, &scaling), 1U);
}

TEST(ElementByteWidthTest, BloblistWidthComesFromFirstSelectionLength)
{
    definition::Scaling scaling;
    scaling.selections = {{"disabled", {0x00, 0x00}}, {"enabled", {0x00, 0x01}}};

    EXPECT_EQ(internal::ElementByteWidth(definition::StorageType::kBloblist, &scaling), 2U);
}

TEST(ElementByteWidthTest, BloblistWidthMatchesLegacySingleByteSelections)
{
    definition::Scaling scaling;
    scaling.selections = {{"disabled", {0x00}}, {"enabled", {0x01}}};

    EXPECT_EQ(internal::ElementByteWidth(definition::StorageType::kBloblist, &scaling), 1U);
}

definition::RomDefinition BlobDefinition()
{
    definition::RomDefinition definition;
    definition::Scaling scaling;
    scaling.name = "modes";
    scaling.storage_type = definition::StorageType::kBloblist;
    scaling.selections = {{"low", {0x01}}, {"high", {0x02}}};
    definition.scalings.push_back(scaling);
    return definition;
}

definition::CalibrationMap BlobMap()
{
    definition::CalibrationMap map;
    map.type = "Selectable";
    map.scaling_name = "modes";
    map.storage_type = definition::StorageType::kBloblist;
    map.address = memory::DefinitionAddress{4};
    return map;
}

auto PatchAt(std::uint64_t address, bytes::Bytes value)
{
    return IsOkAnd(VariantWith<internal::BlobPatch>(
        AllOf(Field(&internal::BlobPatch::address, address), Field(&internal::BlobPatch::bytes, value))));
}

auto Miss(internal::BlobMiss miss)
{
    return IsOkAnd(VariantWith<internal::BlobMiss>(miss));
}

TEST(ResolveBlobSelection, ReturnsTheNamedBytesAtTheMapAddress)
{
    EXPECT_THAT(internal::ResolveBlobSelection(BlobDefinition(), BlobMap(), "high"), PatchAt(4, {0x02}));
    EXPECT_THAT(internal::ResolveBlobSelection(BlobDefinition(), BlobMap(), "low"), PatchAt(4, {0x01}));
}

TEST(ResolveBlobSelection, AMapWithoutAnAddressResolvesToAddressZero)
{
    auto map = BlobMap();
    map.address.reset();
    EXPECT_THAT(internal::ResolveBlobSelection(BlobDefinition(), map, "low"), PatchAt(0, {0x01}));
}

TEST(ResolveBlobSelection, StorageFallsBackToTheScaling)
{
    auto map = BlobMap();
    map.storage_type.reset();
    EXPECT_THAT(internal::ResolveBlobSelection(BlobDefinition(), map, "low"), PatchAt(4, {0x01}));
}

TEST(ResolveBlobSelection, StorageDeclaredOnTheMapAloneIsEnough)
{
    auto definition = BlobDefinition();
    definition.scalings[0].storage_type.reset();
    EXPECT_THAT(internal::ResolveBlobSelection(definition, BlobMap(), "low"), PatchAt(4, {0x01}));
}

TEST(ResolveBlobSelection, NonBlobMapsAreNotABlob)
{
    auto uint8 = BlobDefinition();
    uint8.scalings[0].storage_type = definition::StorageType::kUint8;
    auto uint8_map = BlobMap();
    uint8_map.storage_type = definition::StorageType::kUint8;
    EXPECT_THAT(internal::ResolveBlobSelection(uint8, uint8_map, "low"), Miss(internal::BlobMiss::kNotABlob));

    auto no_selections = BlobDefinition();
    no_selections.scalings[0].selections.clear();
    EXPECT_THAT(internal::ResolveBlobSelection(no_selections, BlobMap(), "low"), Miss(internal::BlobMiss::kNotABlob));

    auto no_scaling = BlobMap();
    no_scaling.scaling_name = "missing";
    EXPECT_THAT(internal::ResolveBlobSelection(BlobDefinition(), no_scaling, "low"),
                Miss(internal::BlobMiss::kNotABlob));
}

TEST(ResolveBlobSelection, AnUnknownNameIsAnUnknownSelection)
{
    EXPECT_THAT(internal::ResolveBlobSelection(BlobDefinition(), BlobMap(), "missing"),
                Miss(internal::BlobMiss::kUnknownSelection));
}

TEST(ResolveBlobSelection, AWidthMismatchIsAnInvalidConfigError)
{
    auto definition = BlobDefinition();
    definition.scalings[0].selections = {{"low", {0x01}}, {"wide", {0x02, 0x03}}};
    EXPECT_THAT(internal::ResolveBlobSelection(definition, BlobMap(), "wide"), IsErr(ErrorKind::kInvalidConfig));
}

} // namespace
} // namespace fastecu::calibration
