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

MapElementSpec Uint8Spec()
{
    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::kUint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    return spec;
}

MapElementSpec SpecFor(definition::StorageType storage_type, std::string_view endian, std::uint64_t address = 0x10)
{
    MapElementSpec spec;
    spec.address = address;
    spec.storage_type = storage_type;
    spec.endian = endian;
    spec.to_byte = "x";
    spec.from_byte = "x";
    return spec;
}

bytes::Bytes RomOf(std::size_t size)
{
    return bytes::Bytes(size, 0);
}

TEST(ReadRawElement, ReadsAnUnsignedByteAtTheIndexedOffset)
{
    auto rom = RomOf(0x40);
    rom[0x12] = 0xAB;

    ASSERT_THAT(ReadRawElement(rom, Uint8Spec(), 2), fastecu::testing::IsOkAnd(0xAB));
}

TEST(ReadRawElement, ReportsInternalWhenTheWindowRunsPastTheRom)
{
    auto rom = RomOf(0x11);

    ASSERT_THAT(ReadRawElement(rom, Uint8Spec(), 8), fastecu::testing::IsErr(ErrorKind::kInternal));
}

TEST(ReadRawElement, ReadsAnUnsignedWordBigEndian)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x12;
    rom[0x11] = 0x34;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kUint16, "big"), 0),
                fastecu::testing::IsOkAnd(0x1234));
}

TEST(ReadRawElement, ReadsAnUnsignedWordLittleEndian)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x34;
    rom[0x11] = 0x12;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kUint16, "little"), 0),
                fastecu::testing::IsOkAnd(0x1234));
}

TEST(ReadRawElement, ReadsAnUnsignedDwordBigEndian)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x12;
    rom[0x11] = 0x34;
    rom[0x12] = 0x56;
    rom[0x13] = 0x78;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kUint32, "big"), 0),
                fastecu::testing::IsOkAnd(0x12345678));
}

TEST(ReadRawElement, ReadsAnUnsignedDwordLittleEndian)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x78;
    rom[0x11] = 0x56;
    rom[0x12] = 0x34;
    rom[0x13] = 0x12;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kUint32, "little"), 0),
                fastecu::testing::IsOkAnd(0x12345678));
}

TEST(ReadRawElement, ReadsASignedByteAsMinusOne)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0xFF;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kInt8, "big"), 0), fastecu::testing::IsOkAnd(-1));
}

// read_raw_element assembles signed multi-byte values from `data_byte`, the
// same endian-aware assembly the unsigned branch above uses -- big-endian
// reads bytes address-order (lowest address = most significant), little-
// endian reads them reverse-address-order (lowest address = least
// significant), then sign_extend() interprets the result for the storage
// width. Confirmed by hand-tracing the loop in map_edit.cpp: for ROM bytes
// 0x01 0x02, a big-endian read folds 0x01 in first (MSB) then 0x02 (LSB) ->
// 0x0102 (258); a little-endian read folds 0x02 in first then 0x01 -> 0x0201
// (513). Neither fixture byte sets the top bit, so sign_extend is a no-op
// here (see ReadsASignedByteAsMinusOne above for the negative-value path at
// width 1).
TEST(ReadRawElement, ReadsASignedWordBigEndian)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kInt16, "big"), 0),
                fastecu::testing::IsOkAnd(0x0102));
}

TEST(ReadRawElement, ReadsASignedWordLittleEndian)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kInt16, "little"), 0),
                fastecu::testing::IsOkAnd(0x0201));
}

TEST(ReadRawElement, ReadsASignedDwordBigEndian)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;
    rom[0x13] = 0x04;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kInt32, "big"), 0),
                fastecu::testing::IsOkAnd(0x01020304));
}

TEST(ReadRawElement, ReadsASignedDwordLittleEndian)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;
    rom[0x13] = 0x04;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kInt32, "little"), 0),
                fastecu::testing::IsOkAnd(0x04030201));
}

// Legacy fills byte_value[k] = rom[byte_address + storagesize - 1 - k] for
// float storage -- the little_or_float branch is always taken when
// storagetype == "float", regardless of spec.endian -- then reads
// map_data_value.float_value out of a union whose float member overlaps
// that same byte_value[4]. On a little-endian host (every supported host)
// the float member's least-significant byte is byte_value[0], so the
// assembled bit pattern is
//   bits = byte_value[0] | byte_value[1]<<8 | byte_value[2]<<16 | byte_value[3]<<24.
// Substituting the fill formula (byte_value[k] = rom[addr+width-1-k]):
//   bits = rom[addr+3] | rom[addr+2]<<8 | rom[addr+1]<<16 | rom[addr+0]<<24,
// i.e. rom[addr+0] holds the float's most-significant byte: floats are read
// as big-endian-in-ROM, regardless of spec.endian. This matches
// decode_numeric_run's documented float handling in
// calibration_service.cpp ("floats were assembled as big-endian regardless
// of the endian field"). The expected bytes below are derived from this
// union semantics directly -- NOT from map_edit.cpp's implementation -- so
// this test actually pins fidelity to legacy rather than the port's own
// internal consistency.
//
// 1.5f's IEEE-754 bit pattern is 0x3FC00000 (sign 0, exponent 0x7F, mantissa
// 0x400000), chosen because it is easy to state and check by hand. Stored
// big-endian, rom[addr+0..+3] = 0x3F, 0xC0, 0x00, 0x00.
TEST(ReadRawElement, ReadsFloatAsBigEndianInRomRegardlessOfEndianField)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x3F;
    rom[0x11] = 0xC0;
    rom[0x12] = 0x00;
    rom[0x13] = 0x00;

    // spec.endian is "little" here specifically to demonstrate it is
    // ignored for float storage.
    const auto value = ReadRawElement(rom, SpecFor(definition::StorageType::kFloat, "little"), 0);

    ASSERT_THAT(value, fastecu::testing::IsOk());
    EXPECT_EQ(static_cast<std::uint32_t>(*value), 0x3FC00000U);
    EXPECT_EQ(std::bit_cast<float>(static_cast<std::uint32_t>(*value)), 1.5F);
}

TEST(ReadRawElement, ReadsAnUnsigned24BitValueCorrectly)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kUint24, "big"), 0),
                fastecu::testing::IsOkAnd(0x010203));
}

// Legacy's signed branch used to test only storagesize 1, 2, and 4 -- a
// 3-byte signed value fell through every `if`, always reading as 0
// regardless of the actual ROM bytes (spec's defect (f), fixed in 6b-4 by
// routing every signed width, including 3, through
// sign_extend(data_byte, width) uniformly). Non-zero ROM bytes are used
// deliberately, to show the correct value is not simply an artifact of an
// all-zero fixture.
TEST(ReadRawElement, ReadsASigned24BitValueCorrectly)
{
    auto rom = RomOf(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;

    ASSERT_THAT(ReadRawElement(rom, SpecFor(definition::StorageType::kInt24, "big"), 0),
                fastecu::testing::IsOkAnd(0x010203));
}

TEST(ElementByteAddress, Wrx02ReadAndWritePredicatesAgreeWhenNeitherRelocates)
{
    MapElementSpec spec = SpecFor(definition::StorageType::kUint8, "big", /*address=*/0x100);
    spec.flash_method = "wrx02";
    spec.rom_file_size = 0x40000; // 256 KiB: >= address, and >= the 190 KiB write threshold.

    EXPECT_EQ(ElementByteAddress(spec, 0, /*for_write=*/false), ElementByteAddress(spec, 0, /*for_write=*/true));
    EXPECT_EQ(ElementByteAddress(spec, 0, /*for_write=*/false), 0x100U);
}

// get_rom_data_value (read) relocates when `rom_file_size < byte_address`.
// set_rom_data_value (write) relocates when `rom_file_size < 190*1024 &&
// byte_address > 0x27FFF`. A 180 KiB image with a cell at 0x28000 satisfies
// the write predicate (180 KiB < 190 KiB and 0x28000 > 0x27FFF) but not the
// read predicate (180 KiB == 0x2D000 > 0x28000, so rom_file_size is NOT less
// than byte_address) -- the two paths disagree on whether to relocate the
// same element. Ported verbatim; spec's defect (a), deliberately left
// unreconciled pending corpus evidence about which predicate real wrx02
// ROMs actually need -- the 6b-4 fix wave fixes the byte-order defects but
// intentionally does not touch this one.
TEST(ElementByteAddress, PinnedDefect_Wrx02FixupDiffersBetweenReadAndWrite)
{
    MapElementSpec spec = SpecFor(definition::StorageType::kUint8, "big", /*address=*/0x28000);
    spec.flash_method = "wrx02";
    spec.rom_file_size = std::uint64_t{180} * 1024;

    EXPECT_NE(ElementByteAddress(spec, 0, /*for_write=*/false), ElementByteAddress(spec, 0, /*for_write=*/true));
}

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
    EXPECT_EQ(ElementByteAddress(spec, 0, false), 0x102U);
    EXPECT_EQ(ElementByteAddress(spec, 1, false), 0x108U);
    EXPECT_EQ(ElementByteAddress(spec, 2, false), 0x10EU);
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
