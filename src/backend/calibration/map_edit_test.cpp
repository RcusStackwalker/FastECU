#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/calibration/map_edit.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace fastecu::calibration
{
namespace
{

// Spec defect (b): the edit path used a flat address + index*width layout while
// decode_numeric_run honours start_position and interval, so editing a
// strided map landed on neighbouring data.
TEST(ElementByteAddress, HonoursTheStartPositionAndIntervalStride)
{
    MapElementSpec spec;
    spec.address = 0x100;
    spec.storage_type = definition::StorageType::kUint16;
    spec.start_position = 2;
    spec.interval = 3;

    // addr(j) = 0x100 + (2-1)*2 + j*2*3
    EXPECT_EQ(ElementByteAddress(spec, 0), 0x102U);
    EXPECT_EQ(ElementByteAddress(spec, 1), 0x108U);
    EXPECT_EQ(ElementByteAddress(spec, 2), 0x10EU);
}

TEST(WriteRawElement, WritesInTheLabeledByteOrderForEveryWidth)
{
    struct Case
    {
        definition::StorageType storage;
        std::string_view endian;
        std::int64_t raw;
        std::vector<std::uint8_t> expected;
    };
    const std::vector<Case> cases = {
        {definition::StorageType::kUint8, "big", 0xAB, {0xAB}},
        {definition::StorageType::kUint8, "little", 0xAB, {0xAB}},
        {definition::StorageType::kUint16, "big", 0x1234, {0x12, 0x34}},
        {definition::StorageType::kUint16, "little", 0x1234, {0x34, 0x12}},
        {definition::StorageType::kUint24, "big", 0x123456, {0x12, 0x34, 0x56}},
        {definition::StorageType::kUint24, "little", 0x123456, {0x56, 0x34, 0x12}},
        {definition::StorageType::kUint32, "big", 0x12345678, {0x12, 0x34, 0x56, 0x78}},
        {definition::StorageType::kUint32, "little", 0x12345678, {0x78, 0x56, 0x34, 0x12}},
        {definition::StorageType::kInt8, "big", -2, {0xFE}},
        {definition::StorageType::kInt16, "big", -300, {0xFE, 0xD4}},
        {definition::StorageType::kInt32, "big", -70000, {0xFF, 0xFE, 0xEE, 0x90}},
        // Float: raw is a BIT PATTERN, not a number to convert -- the encoded
        // bits of 1.5F supplied directly to the byte-packing primitive.
        // "little" is used deliberately to show the endian label is ignored
        // for float storage.
        {definition::StorageType::kFloat,
         "little",
         static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(1.5F)),
         {0x3F, 0xC0, 0x00, 0x00}},
    };

    for (const auto& c : cases)
    {
        SCOPED_TRACE(::testing::PrintToString(c));
        MapElementSpec spec;
        spec.storage_type = c.storage;
        spec.endian = c.endian;

        const auto written = WriteRawElement(spec, c.raw);
        ASSERT_THAT(written, fastecu::testing::IsOk());

        EXPECT_EQ(*written, c.expected) << "storage=" << definition::StorageTypeText(c.storage)
                                        << " endian=" << c.endian;
    }
}

TEST(ResolveEditTarget, LeftColumnSelectionOnAMultiRowMapTargetsTheYAxis)
{
    // Widget column 0 is the Y-axis header column.
    const auto target = ResolveEditTarget({.first_row = 1, .first_col = 0, .last_row = 2, .last_col = 0},
                                          {.x_size = 4, .y_size = 4}, "Y Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kYAxis);
    // Legacy subtracts 1 from every bound, then adds 1 back to both columns.
    EXPECT_EQ(target.range.first_col, 0);
    EXPECT_EQ(target.range.last_col, 0);
    EXPECT_EQ(target.range.first_row, 0);
    // The Y axis is one element wide regardless of the map's x_size.
    EXPECT_EQ(target.x_size, 1U);
}

TEST(ResolveEditTarget, TopRowSelectionOnAMultiColumnMapTargetsTheXAxis)
{
    const auto target = ResolveEditTarget({.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 3},
                                          {.x_size = 4, .y_size = 4}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kXAxis);
    EXPECT_EQ(target.range.first_row, 0);
    EXPECT_EQ(target.x_size, 4U);
}

TEST(ResolveEditTarget, XAxisSelectionOnAMultiRowMapShiftsColumnsPastTheYAxisHeader)
{
    // y_size > 1 reserves widget column 0 for the Y axis, so widget columns
    // 1..3 are X-axis elements 0..2.
    const auto target = ResolveEditTarget({.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 3},
                                          {.x_size = 4, .y_size = 4}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kXAxis);
    EXPECT_EQ(target.range.first_col, 0);
    EXPECT_EQ(target.range.last_col, 2);
}

TEST(ResolveEditTarget, XAxisSelectionOnASingleRowMapKeepsColumnsInRange)
{
    // y_size == 1 has no Y-axis header column, so widget column 0 is X-axis
    // element 0. It used to resolve to column -1 and index cell values out of
    // bounds.
    const auto target = ResolveEditTarget({.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 2},
                                          {.x_size = 8, .y_size = 1}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kXAxis);
    EXPECT_EQ(target.range.first_row, 0);
    EXPECT_EQ(target.range.last_row, 0);
    EXPECT_EQ(target.range.first_col, 0);
    EXPECT_EQ(target.range.last_col, 2);
    EXPECT_EQ(target.x_size, 8U);
}

TEST(ResolveEditTarget, StaticScaleTypesRejectAnAxisEdit)
{
    for (const std::string_view type : {"Static X Axis", "Static Y Axis"})
    {
        SCOPED_TRACE(::testing::PrintToString(type));
        const auto target = ResolveEditTarget({.first_row = 1, .first_col = 0, .last_row = 2, .last_col = 0},
                                              {.x_size = 4, .y_size = 4}, type);
        EXPECT_EQ(target.kind, EditTargetKind::kRejected) << type;
    }
}

TEST(ResolveEditTarget, StaticScaleTypesRejectAnXAxisEdit)
{
    // first_row == 0 with x_size > 1, and first_col != 0 so the Y-axis
    // branch's first_col == 0 check does not fire first -- this exercises
    // the X-axis branch's own is_static_scale rejection, which
    // StaticScaleTypesRejectAnAxisEdit above never reaches (its selection has
    // first_col == 0, so both loop iterations take the Y-axis branch).
    const auto target = ResolveEditTarget({.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 1},
                                          {.x_size = 4, .y_size = 4}, "Static X Axis");
    EXPECT_EQ(target.kind, EditTargetKind::kRejected);
}

TEST(ResolveEditTarget, SingleColumnMapShiftsRowsBackIntoRange)
{
    // x_size == 1 and a non-static scale type: legacy adds 1 to both rows.
    const auto target = ResolveEditTarget({.first_row = 1, .first_col = 1, .last_row = 2, .last_col = 1},
                                          {.x_size = 1, .y_size = 8}, "Y Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kMapBody);
    EXPECT_EQ(target.range.first_row, 1);
    EXPECT_EQ(target.range.last_row, 2);
}

TEST(ResolveEditTarget, SingleRowMapShiftsColumnsBackIntoRange)
{
    // y_size == 1 and a non-static scale type: legacy adds 1 to both columns,
    // symmetric to SingleColumnMapShiftsRowsBackIntoRange above.
    const auto target = ResolveEditTarget({.first_row = 1, .first_col = 1, .last_row = 1, .last_col = 2},
                                          {.x_size = 8, .y_size = 1}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kMapBody);
    EXPECT_EQ(target.range.first_col, 1);
    EXPECT_EQ(target.range.last_col, 2);
}

TEST(ResolveEditTarget, BodySelectionOnAMapThatIsNeitherOneByNNorNByOne)
{
    // Neither axis condition is met (selection doesn't touch row/column 0)
    // and neither dimension is 1, so no branch fires: a plain `-1` shift.
    const auto target = ResolveEditTarget({.first_row = 2, .first_col = 2, .last_row = 3, .last_col = 3},
                                          {.x_size = 4, .y_size = 4}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kMapBody);
    EXPECT_EQ(target.range.first_row, 1);
    EXPECT_EQ(target.range.first_col, 1);
    EXPECT_EQ(target.range.last_row, 2);
    EXPECT_EQ(target.range.last_col, 2);
    EXPECT_EQ(target.x_size, 4U);
}

TEST(ResolveEditTarget, WidgetRowZeroOnASingleColumnMapFallsThroughToTheBodyBranch)
{
    // The X-axis branch requires x_size > 1; with x_size == 1, a selection
    // at widget row 0 falls through to the body branch instead, which itself
    // shifts rows back because x_size == 1 (no row-0 header to reserve).
    const auto target = ResolveEditTarget({.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 1},
                                          {.x_size = 1, .y_size = 4}, "Y Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kMapBody);
    EXPECT_EQ(target.range.first_row, 0);
    EXPECT_EQ(target.range.last_row, 0);
}

// "Empty" here means a selection whose element-coordinate range spans zero
// elements after adjustment (last < first in both dimensions). A live
// QTableWidgetSelectionRange can't actually produce this -- its
// rightColumn()/bottomRow() are never less than leftColumn()/topRow() -- but
// resolve_edit_target has no such invariant to lean on, so this pins the
// degenerate-input behavior: it still resolves mechanically from the
// boundary fields, with no special-casing for a zero-count range.
TEST(ResolveEditTarget, EmptySelectionProducesAZeroCountElementRange)
{
    const auto target = ResolveEditTarget({.first_row = 1, .first_col = 1, .last_row = 0, .last_col = 0},
                                          {.x_size = 4, .y_size = 4}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::kMapBody);
    EXPECT_EQ(target.range.first_row, 0);
    EXPECT_EQ(target.range.last_row, -1);
    EXPECT_EQ(target.range.first_col, 0);
    EXPECT_EQ(target.range.last_col, -1);
}

} // namespace
} // namespace fastecu::calibration
