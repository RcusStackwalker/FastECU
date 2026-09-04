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

MapElementSpec uint8_spec()
{
    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    return spec;
}

MapElementSpec spec_for(definition::StorageType storage_type, std::string_view endian, std::uint64_t address = 0x10)
{
    MapElementSpec spec;
    spec.address = address;
    spec.storage_type = storage_type;
    spec.endian = endian;
    spec.to_byte = "x";
    spec.from_byte = "x";
    return spec;
}

std::vector<std::uint8_t> rom_of(std::size_t size)
{
    std::vector<std::uint8_t> rom(size, 0x00);
    return rom;
}

TEST(ReadRawElement, ReadsAnUnsignedByteAtTheIndexedOffset)
{
    auto rom = rom_of(0x40);
    rom[0x12] = 0xAB;

    const auto value = read_raw_element(rom, uint8_spec(), 2);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0xAB);
}

TEST(ReadRawElement, ReportsInternalWhenTheWindowRunsPastTheRom)
{
    auto rom = rom_of(0x11);

    const auto value = read_raw_element(rom, uint8_spec(), 8);

    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().kind, ErrorKind::Internal);
}

TEST(ReadRawElement, ReadsAnUnsignedWordBigEndian)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x12;
    rom[0x11] = 0x34;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Uint16, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x1234);
}

TEST(ReadRawElement, ReadsAnUnsignedWordLittleEndian)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x34;
    rom[0x11] = 0x12;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Uint16, "little"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x1234);
}

TEST(ReadRawElement, ReadsAnUnsignedDwordBigEndian)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x12;
    rom[0x11] = 0x34;
    rom[0x12] = 0x56;
    rom[0x13] = 0x78;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Uint32, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x12345678);
}

TEST(ReadRawElement, ReadsAnUnsignedDwordLittleEndian)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x78;
    rom[0x11] = 0x56;
    rom[0x12] = 0x34;
    rom[0x13] = 0x12;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Uint32, "little"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x12345678);
}

TEST(ReadRawElement, ReadsASignedByteAsMinusOne)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0xFF;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int8, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, -1);
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
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int16, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x0102);
}

TEST(ReadRawElement, ReadsASignedWordLittleEndian)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int16, "little"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x0201);
}

TEST(ReadRawElement, ReadsASignedDwordBigEndian)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;
    rom[0x13] = 0x04;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int32, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x01020304);
}

TEST(ReadRawElement, ReadsASignedDwordLittleEndian)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;
    rom[0x13] = 0x04;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int32, "little"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x04030201);
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
// decode_scaled_values's documented float handling in
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
    auto rom = rom_of(0x20);
    rom[0x10] = 0x3F;
    rom[0x11] = 0xC0;
    rom[0x12] = 0x00;
    rom[0x13] = 0x00;

    // spec.endian is "little" here specifically to demonstrate it is
    // ignored for float storage.
    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Float, "little"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(static_cast<std::uint32_t>(*value), 0x3FC00000U);
    EXPECT_EQ(std::bit_cast<float>(static_cast<std::uint32_t>(*value)), 1.5F);
}

TEST(ReadRawElement, ReadsAnUnsigned24BitValueCorrectly)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Uint24, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x010203);
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
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int24, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x010203);
}

TEST(ElementByteAddress, Wrx02ReadAndWritePredicatesAgreeWhenNeitherRelocates)
{
    MapElementSpec spec = spec_for(definition::StorageType::Uint8, "big", /*address=*/0x100);
    spec.flash_method = "wrx02";
    spec.rom_file_size = 0x40000; // 256 KiB: >= address, and >= the 190 KiB write threshold.

    EXPECT_EQ(element_byte_address(spec, 0, /*for_write=*/false), element_byte_address(spec, 0, /*for_write=*/true));
    EXPECT_EQ(element_byte_address(spec, 0, /*for_write=*/false), 0x100U);
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
    MapElementSpec spec = spec_for(definition::StorageType::Uint8, "big", /*address=*/0x28000);
    spec.flash_method = "wrx02";
    spec.rom_file_size = std::uint64_t{180} * 1024;

    EXPECT_NE(element_byte_address(spec, 0, /*for_write=*/false), element_byte_address(spec, 0, /*for_write=*/true));
}

// The encode/decode round trip is the safety net for every edit operation: if
// encode_scaled_value and read_raw_element disagree about byte order, an edit
// silently writes a different value than the grid displays.
//
// Every case here round-trips: every unsigned width at both endians, every
// signed width at both endians (added once the 6b-4 fix made
// read_raw_element assemble signed multi-byte values from the same
// correctly-endian-assembled `data_byte` the unsigned path already used,
// eliminating the byte-swap that used to make these NOT round-trip -- see
// the deleted PinnedDefect_SignedMultiByteDoesNotRoundTripBecauseTheReadIsByteSwapped,
// whose premise this fix falsifies), and Int24, previously untestable here
// because it always read back as 0.
TEST(EncodeScaledValue, RoundTripsThroughReadRawElementForEveryWidth)
{
    struct Case
    {
        definition::StorageType storage;
        std::string_view endian;
        std::int64_t raw;
    };
    const Case cases[] = {
        {definition::StorageType::Uint8, "big", 0xAB},           {definition::StorageType::Uint16, "big", 0x1234},
        {definition::StorageType::Uint16, "little", 0x1234},     {definition::StorageType::Uint24, "big", 0x123456},
        {definition::StorageType::Uint24, "little", 0x123456},   {definition::StorageType::Uint32, "big", 0x12345678},
        {definition::StorageType::Uint32, "little", 0x12345678}, {definition::StorageType::Int8, "big", -2},
        {definition::StorageType::Int16, "big", -300},           {definition::StorageType::Int16, "little", -300},
        {definition::StorageType::Int32, "big", -70000},         {definition::StorageType::Int32, "little", -70000},
        {definition::StorageType::Int24, "big", 0x010203},
    };

    for (const auto& c : cases)
    {
        MapElementSpec spec;
        spec.address = 0x10;
        spec.storage_type = c.storage;
        spec.endian = c.endian;
        spec.to_byte = "x";
        spec.from_byte = "x";

        const auto encoded = encode_scaled_value(spec, double(c.raw), 15);
        ASSERT_TRUE(encoded.has_value()) << to_string(encoded.error().kind);

        std::vector<std::uint8_t> rom(0x40, 0x00);
        std::ranges::copy(*encoded, rom.begin() + 0x10);

        const auto decoded = read_raw_element(rom, spec, 0);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, c.raw) << "storage=" << definition::storage_type_text(c.storage) << " endian=" << c.endian;
    }
}

// encode_scaled_value writes the byte order spec.endian's label claims,
// matching decode_scaled_values (the correct decoder) and
// read_raw_element's own endian handling -- the write side of the same
// self-consistency read_raw_element's byte-swap fix restored. (Legacy
// set_rom_data_value's write order used to be measured as INVERTED relative
// to its own endian label -- raw 0x1234 labeled "big" wrote [0x34, 0x12] --
// a divergence this fix wave reconciles rather than perpetuates; see the
// deleted PinnedDefect_WriteOrderDivergesFromLegacyBecauseLegacyIsAlsoByteSwapped
// and LegacyByteOrderFlagSelectsWriteOrder, which exercised the now-removed
// legacy_byte_order flag.) Float storage is always written
// big-endian-in-ROM regardless of spec.endian.
TEST(EncodeScaledValue, EncodesInTheLabeledByteOrder)
{
    MapElementSpec spec;
    spec.storage_type = definition::StorageType::Uint16;
    spec.endian = "big";
    spec.to_byte = "x";

    const auto encoded = encode_scaled_value(spec, 0x1234, 15);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(*encoded, (std::vector<std::uint8_t>{0x12, 0x34}));

    MapElementSpec float_spec;
    float_spec.storage_type = definition::StorageType::Float;
    float_spec.endian = "little"; // deliberately "little" to show it's ignored
    float_spec.to_byte = "x";

    const auto float_encoded = encode_scaled_value(float_spec, 1.5, 15);
    ASSERT_TRUE(float_encoded.has_value());
    EXPECT_EQ(*float_encoded, (std::vector<std::uint8_t>{0x3F, 0xC0, 0x00, 0x00}));
}

// write_raw_element is the byte-packing half of encode_scaled_value, and is
// itself the faithful port of legacy set_rom_data_value -- a pure byte
// writer with no expression evaluation and no rounding of its own. Expected
// bytes below are independently derived (not read off write_raw_element's
// own output) from the shift formula the byte order label implies:
// little_endian ? 8*k : 8*(width-1-k) for each output byte k, with
// little_endian = !is_float && (endian == "little") -- e.g. uint16 "big"
// 0x1234 places the most-significant byte (0x12) first, uint16 "little"
// 0x1234 places the least-significant byte (0x34) first; a negative raw's
// low 32 bits are packed as an unsigned bit pattern (e.g. -300 packs as
// 0xFFFFFED4, so "big" emits 0xFE then 0xD4); float storage is always
// packed most-significant-byte-first regardless of the endian label.
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
        {definition::StorageType::Uint8, "big", 0xAB, {0xAB}},
        {definition::StorageType::Uint8, "little", 0xAB, {0xAB}},
        {definition::StorageType::Uint16, "big", 0x1234, {0x12, 0x34}},
        {definition::StorageType::Uint16, "little", 0x1234, {0x34, 0x12}},
        {definition::StorageType::Uint24, "big", 0x123456, {0x12, 0x34, 0x56}},
        {definition::StorageType::Uint24, "little", 0x123456, {0x56, 0x34, 0x12}},
        {definition::StorageType::Uint32, "big", 0x12345678, {0x12, 0x34, 0x56, 0x78}},
        {definition::StorageType::Uint32, "little", 0x12345678, {0x78, 0x56, 0x34, 0x12}},
        {definition::StorageType::Int8, "big", -2, {0xFE}},
        {definition::StorageType::Int16, "big", -300, {0xFE, 0xD4}},
        {definition::StorageType::Int32, "big", -70000, {0xFF, 0xFE, 0xEE, 0x90}},
        // Float: raw is a BIT PATTERN, not a number to convert -- the encoded
        // bits of 1.5F, exactly what encode_scaled_value's bit_cast produces
        // and what legacy's callers smuggled through the float parameter.
        // "little" is used deliberately to show the endian label is ignored
        // for float storage.
        {definition::StorageType::Float,
         "little",
         static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(1.5F)),
         {0x3F, 0xC0, 0x00, 0x00}},
    };

    for (const auto& c : cases)
    {
        MapElementSpec spec;
        spec.storage_type = c.storage;
        spec.endian = c.endian;

        const auto written = write_raw_element(spec, c.raw);
        ASSERT_TRUE(written.has_value()) << to_string(written.error().kind);

        EXPECT_EQ(*written, c.expected) << "storage=" << definition::storage_type_text(c.storage)
                                        << " endian=" << c.endian;
    }
}

TEST(ResolveEditTarget, LeftColumnSelectionOnAMultiRowMapTargetsTheYAxis)
{
    // Widget column 0 is the Y-axis header column.
    const auto target = resolve_edit_target({.first_row = 1, .first_col = 0, .last_row = 2, .last_col = 0},
                                            {.x_size = 4, .y_size = 4}, "Y Axis");

    EXPECT_EQ(target.kind, EditTargetKind::YAxis);
    // Legacy subtracts 1 from every bound, then adds 1 back to both columns.
    EXPECT_EQ(target.range.first_col, 0);
    EXPECT_EQ(target.range.last_col, 0);
    EXPECT_EQ(target.range.first_row, 0);
    // The Y axis is one element wide regardless of the map's x_size.
    EXPECT_EQ(target.x_size, 1U);
}

TEST(ResolveEditTarget, TopRowSelectionOnAMultiColumnMapTargetsTheXAxis)
{
    const auto target = resolve_edit_target({.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 3},
                                            {.x_size = 4, .y_size = 4}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::XAxis);
    EXPECT_EQ(target.range.first_row, 0);
    EXPECT_EQ(target.x_size, 4U);
}

TEST(ResolveEditTarget, StaticScaleTypesRejectAnAxisEdit)
{
    for (const std::string_view type : {"Static X Axis", "Static Y Axis"})
    {
        const auto target = resolve_edit_target({.first_row = 1, .first_col = 0, .last_row = 2, .last_col = 0},
                                                {.x_size = 4, .y_size = 4}, type);
        EXPECT_EQ(target.kind, EditTargetKind::Rejected) << type;
    }
}

TEST(ResolveEditTarget, StaticScaleTypesRejectAnXAxisEdit)
{
    // first_row == 0 with x_size > 1, and first_col != 0 so the Y-axis
    // branch's first_col == 0 check does not fire first -- this exercises
    // the X-axis branch's own is_static_scale rejection, which
    // StaticScaleTypesRejectAnAxisEdit above never reaches (its selection has
    // first_col == 0, so both loop iterations take the Y-axis branch).
    const auto target = resolve_edit_target({.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 1},
                                            {.x_size = 4, .y_size = 4}, "Static X Axis");
    EXPECT_EQ(target.kind, EditTargetKind::Rejected);
}

TEST(ResolveEditTarget, SingleColumnMapShiftsRowsBackIntoRange)
{
    // x_size == 1 and a non-static scale type: legacy adds 1 to both rows.
    const auto target = resolve_edit_target({.first_row = 1, .first_col = 1, .last_row = 2, .last_col = 1},
                                            {.x_size = 1, .y_size = 8}, "Y Axis");

    EXPECT_EQ(target.kind, EditTargetKind::MapBody);
    EXPECT_EQ(target.range.first_row, 1);
    EXPECT_EQ(target.range.last_row, 2);
}

TEST(ResolveEditTarget, SingleRowMapShiftsColumnsBackIntoRange)
{
    // y_size == 1 and a non-static scale type: legacy adds 1 to both columns,
    // symmetric to SingleColumnMapShiftsRowsBackIntoRange above.
    const auto target = resolve_edit_target({.first_row = 1, .first_col = 1, .last_row = 1, .last_col = 2},
                                            {.x_size = 8, .y_size = 1}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::MapBody);
    EXPECT_EQ(target.range.first_col, 1);
    EXPECT_EQ(target.range.last_col, 2);
}

TEST(ResolveEditTarget, BodySelectionOnAMapThatIsNeitherOneByNNorNByOne)
{
    // Neither axis condition is met (selection doesn't touch row/column 0)
    // and neither dimension is 1, so no branch fires: a plain `-1` shift.
    const auto target = resolve_edit_target({.first_row = 2, .first_col = 2, .last_row = 3, .last_col = 3},
                                            {.x_size = 4, .y_size = 4}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::MapBody);
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
    const auto target = resolve_edit_target({.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 1},
                                            {.x_size = 1, .y_size = 4}, "Y Axis");

    EXPECT_EQ(target.kind, EditTargetKind::MapBody);
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
    const auto target = resolve_edit_target({.first_row = 1, .first_col = 1, .last_row = 0, .last_col = 0},
                                            {.x_size = 4, .y_size = 4}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::MapBody);
    EXPECT_EQ(target.range.first_row, 0);
    EXPECT_EQ(target.range.last_row, -1);
    EXPECT_EQ(target.range.first_col, 0);
    EXPECT_EQ(target.range.last_col, -1);
}

TEST(MapValueDecimalCount, CountsZerosAfterTheDecimalPoint)
{
    EXPECT_EQ(map_value_decimal_count("0.00"), 2);
    EXPECT_EQ(map_value_decimal_count("0.000"), 3);
    EXPECT_EQ(map_value_decimal_count("0"), 0);
    EXPECT_EQ(map_value_decimal_count(""), 0);
}

TEST(ApplyIncrement, AddsTheCoarseStepToEverySelectedCell)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 10;
    rom[0x11] = 20;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 5.0;
    spec.fine_increment = 1.0;
    spec.x_size = 2;
    spec.y_size = 1;

    const std::string_view cells[] = {"10", "20"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/2, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 1},
                        IncrementStep::CoarseUp, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 2U);
    EXPECT_EQ((*patch)[0].display_text, "15");
    EXPECT_EQ((*patch)[0].bytes, std::vector<std::uint8_t>{15});
    EXPECT_EQ((*patch)[1].display_text, "25");
}

TEST(ApplyIncrement, ClampsToTheDefinitionMaximum)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 250;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 100.0;
    spec.fine_increment = 1.0;
    spec.max_value = "255";
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"250"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/1, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                        IncrementStep::CoarseUp, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ((*patch)[0].display_text, "255");
}

// Spec defect (d), fixed by the extraction: legacy raised a modal inside a
// retry loop that a zero increment could never exit.
TEST(ApplyIncrement, ReportsInvalidConfigWhenTheIncrementIsZero)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 0.0;
    spec.fine_increment = 0.0;
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"0"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/1, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                        IncrementStep::CoarseUp, 15);

    ASSERT_FALSE(patch.has_value());
    EXPECT_EQ(patch.error().kind, ErrorKind::InvalidConfig);
}

// The four saturation/sign-wrap guard tests below assert that an increment
// which would overflow the storage width leaves the cell at its previous raw
// value -- none of them set min_value/max_value, so the guard (not the
// definition clamp) is what's under test in each case.

TEST(ApplyIncrement, SaturationGuardLeavesUint8AtItsPreviousValuePastMax)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 250;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 10.0;
    spec.fine_increment = 1.0;
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"250"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/1, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                        IncrementStep::CoarseUp, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ((*patch)[0].bytes, std::vector<std::uint8_t>{250});
    EXPECT_EQ((*patch)[0].display_text, "250");
}

TEST(ApplyIncrement, SaturationGuardLeavesInt8AtItsPreviousValueCrossingSignBoundary)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 100; // 0x64, positive as int8.

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Int8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 50.0;
    spec.fine_increment = 1.0;
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"100"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/1, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                        IncrementStep::CoarseUp, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ((*patch)[0].bytes, std::vector<std::uint8_t>{100});
    EXPECT_EQ((*patch)[0].display_text, "100");
}

TEST(ApplyIncrement, SaturationGuardLeavesInt16AtItsPreviousValueCrossingSignBoundary)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    // 30000 = 0x7530, positive as int16.
    rom[0x10] = 0x75;
    rom[0x11] = 0x30;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Int16;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 10000.0;
    spec.fine_increment = 1.0;
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"30000"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/1, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                        IncrementStep::CoarseUp, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ((*patch)[0].bytes, (std::vector<std::uint8_t>{0x75, 0x30}));
    EXPECT_EQ((*patch)[0].display_text, "30000");
}

TEST(ApplyIncrement, SaturationGuardLeavesUnsignedStorageAtItsPreviousValueOnANegativeResult)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 0;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 5.0;
    spec.fine_increment = 1.0;
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"0"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/1, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                        IncrementStep::CoarseDown, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ((*patch)[0].bytes, std::vector<std::uint8_t>{0});
    EXPECT_EQ((*patch)[0].display_text, "0");
}

// Spec defect (d)'s real fix: the retry is preserved (bounded, not removed)
// so a to_byte scaling coarser than 1:1 doesn't silently no-op the edit --
// see apply_increment's doc comment.
TEST(ApplyIncrement, RetriesUntilTheEncodedValueActuallyChanges)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 5; // to_byte("10") = 10/2 = 5

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x/2";
    spec.from_byte = "x*2";
    spec.coarse_increment = 5.0;
    spec.fine_increment = 0.5;
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"10"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/1, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                        IncrementStep::FineUp, 15);

    ASSERT_TRUE(patch.has_value());
    // Attempt 1: display 10.5 -> to_byte -> 5.25 -> llround -> 5 (unchanged, retry).
    // Attempt 2: display 11.0 -> to_byte -> 5.5  -> llround -> 6 (changed, stop).
    // display_text reflects the FINAL raw's from_byte, not the intermediate
    // display value -- this is the "snaps" behavior the retry exists for.
    EXPECT_EQ((*patch)[0].display_text, "12");
    EXPECT_EQ((*patch)[0].bytes, (std::vector<std::uint8_t>{6}));
}

TEST(ApplyIncrement, ReportsInvalidConfigWhenTheRetryBoundIsExhausted)
{
    std::vector<std::uint8_t> rom(0x40, 0x00); // rom[0x10] = 0

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "0"; // constant -- ignores its input entirely
    spec.from_byte = "x";
    spec.coarse_increment = 5.0;
    spec.fine_increment = 1.0;
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"0"};
    const auto patch =
        apply_increment(rom, spec, /*x_size=*/1, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                        IncrementStep::CoarseUp, 15);

    ASSERT_FALSE(patch.has_value());
    EXPECT_EQ(patch.error().kind, ErrorKind::InvalidConfig);
}

TEST(ApplySetExpression, AppliesEachOperatorToEveryCell)
{
    struct Case
    {
        std::string_view input;
        std::string_view expected;
    };
    const Case cases[] = {
        {"+5", "15"}, {"-5", "5"}, {"*2", "20"}, {"/2", "5"}, {"42", "42"},
    };

    for (const auto& c : cases)
    {
        std::vector<std::uint8_t> rom(0x40, 0x00);
        rom[0x10] = 10;

        MapElementSpec spec;
        spec.address = 0x10;
        spec.storage_type = definition::StorageType::Uint8;
        spec.endian = "big";
        spec.to_byte = "x";
        spec.from_byte = "x";
        spec.x_size = 1;
        spec.y_size = 1;

        const std::string_view cells[] = {"10"};
        const auto patch =
            apply_set_expression(rom, spec, /*x_size=*/1, cells,
                                 {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, c.input, 15);

        ASSERT_TRUE(patch.has_value()) << c.input;
        EXPECT_EQ((*patch)[0].display_text, c.expected) << c.input;
    }
}

TEST(ApplySetExpression, ReportsInvalidConfigOnDivisionByZero)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 10;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"10"};
    const auto patch = apply_set_expression(rom, spec, /*x_size=*/1, cells,
                                            {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, "/0", 15);

    ASSERT_FALSE(patch.has_value());
    EXPECT_EQ(patch.error().kind, ErrorKind::InvalidConfig);
}

// Spec defect (c), fixed: set_value now runs the same storage-type
// saturation guard apply_increment does, via the shared encode_guarded path.
TEST(ApplySetExpression, RejectsAValueThatWouldOverflowTheStorageType)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 10;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"10"};
    const auto patch = apply_set_expression(rom, spec, /*x_size=*/1, cells,
                                            {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, "300", 15);

    // 300 overflows a uint8 -- the whole call fails instead of truncating
    // silently into the storage type.
    ASSERT_FALSE(patch.has_value());
    EXPECT_EQ(patch.error().kind, ErrorKind::InvalidConfig);
}

MapElementSpec linear_uint8_spec(std::uint32_t x_size, std::uint32_t y_size)
{
    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.x_size = x_size;
    spec.y_size = y_size;
    return spec;
}

TEST(ApplyInterpolation, HorizontalFillsEachRowLinearlyBetweenItsEndpoints)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "99", "99", "30"};

    const auto patch = apply_interpolation(rom, linear_uint8_spec(4, 1), /*x_size=*/4, cells,
                                           {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 3},
                                           InterpolationMode::Horizontal, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 4U);
    EXPECT_EQ((*patch)[0].display_text, "0");
    EXPECT_EQ((*patch)[1].display_text, "10");
    EXPECT_EQ((*patch)[2].display_text, "20");
    EXPECT_EQ((*patch)[3].display_text, "30");
}

// Spec defect (e), fixed by the extraction: legacy indexed a fixed
// float[128][128] with the raw selection extent, so any selection wider or
// taller than 128 wrote past the array.
TEST(ApplyInterpolation, HandlesASelectionWiderThanTheLegacyFixedArray)
{
    constexpr std::uint32_t kWidth = 200;
    std::vector<std::uint8_t> rom(0x10 + kWidth, 0x00);
    std::vector<std::string> owned(kWidth, "0");
    owned.front() = "0";
    owned.back() = "199";
    std::vector<std::string_view> cells(owned.begin(), owned.end());

    const auto patch = apply_interpolation(rom, linear_uint8_spec(kWidth, 1), /*x_size=*/kWidth, cells,
                                           {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = kWidth - 1},
                                           InterpolationMode::Horizontal, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->size(), kWidth);
    EXPECT_EQ(patch->back().display_text, "199");
}

TEST(ApplyInterpolation, VerticalFillsEachColumnLinearlyBetweenItsEndpoints)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "99", "99", "30"};

    const auto patch = apply_interpolation(rom, linear_uint8_spec(1, 4), /*x_size=*/1, cells,
                                           {.first_row = 0, .first_col = 0, .last_row = 3, .last_col = 0},
                                           InterpolationMode::Vertical, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 4U);
    EXPECT_EQ((*patch)[0].display_text, "0");
    EXPECT_EQ((*patch)[1].display_text, "10");
    EXPECT_EQ((*patch)[2].display_text, "20");
    EXPECT_EQ((*patch)[3].display_text, "30");
}

// A 3x3 selection with all four corners set; the centre cell's expected
// value (80) is the standard bilinear interpolation of the four corners at
// the selection's midpoint ((0*0.25)+(20*0.25)+(100*0.25)+(200*0.25) = 80),
// confirming bidirectional's two-pass edges-then-rows structure reduces to
// the textbook bilinear result. The nine non-corner cells' seed text is
// irrelevant -- bidirectional's second pass overwrites every cell in each
// row (including the corners' own row) from that row's freshly interpolated
// edges.
TEST(ApplyInterpolation, BidirectionalCentreCellIsTheBilinearResultOfTheFourCorners)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    // clang-format off
    const std::string_view cells[] = {
        "0",   "0", "20",
        "0",   "0",  "0",
        "100", "0", "200",
    };
    // clang-format on

    const auto patch = apply_interpolation(rom, linear_uint8_spec(3, 3), /*x_size=*/3, cells,
                                           {.first_row = 0, .first_col = 0, .last_row = 2, .last_col = 2},
                                           InterpolationMode::Bidirectional, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 9U);
    EXPECT_EQ((*patch)[4].display_text, "80");
}

TEST(ApplyPaste, WritesTheClipboardBlockAnchoredAtTheSelectionCorner)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "0", "0", "0"};
    const std::vector<std::string_view> rows[] = {{"11", "22"}};

    const auto patch = apply_paste(rom, linear_uint8_spec(2, 2), /*x_size=*/2, /*y_size=*/2, cells,
                                   {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 1}, rows, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 2U);
    EXPECT_EQ((*patch)[0].display_text, "11");
    EXPECT_EQ((*patch)[1].display_text, "22");
}

TEST(ApplyPaste, DropsCellsThatFallOutsideTheMap)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "0", "0", "0"};
    // Three columns pasted into a two-column map: legacy silently drops the
    // third rather than reporting. Exercises the x_size half of the
    // two-dimension bounds check; DropsRowsThatFallOutsideTheMap below
    // exercises the y_size half.
    const std::vector<std::string_view> rows[] = {{"11", "22", "33"}};

    const auto patch = apply_paste(rom, linear_uint8_spec(2, 2), /*x_size=*/2, /*y_size=*/2, cells,
                                   {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 1}, rows, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->size(), 2U);
}

// apply_paste's bounds check has two independent halves -- dest_row >=
// y_size and dest_col >= x_size -- and is exactly the class of check prone to
// a row/col or x_size/y_size swap. DropsCellsThatFallOutsideTheMap above
// exercises only the x_size half (an extra COLUMN); this test is its
// transpose, pasting an extra ROW into a map with too few rows, so a swap in
// either the comparison or the dest_row/dest_col computation would be caught
// here even if it slipped past the column-only test.
TEST(ApplyPaste, DropsRowsThatFallOutsideTheMap)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "0", "0", "0"};
    // Three rows pasted into a two-row map.
    const std::vector<std::string_view> rows[] = {{"11"}, {"22"}, {"33"}};

    const auto patch = apply_paste(rom, linear_uint8_spec(2, 2), /*x_size=*/2, /*y_size=*/2, cells,
                                   {.first_row = 0, .first_col = 0, .last_row = 2, .last_col = 0}, rows, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->size(), 2U);
}

// Spec defect (c), fixed: paste now clamps to the definition's min/max and
// runs the same saturation guard the other three operations do, via the
// shared encode_guarded path. Clamping necessarily makes display_text derive
// from the (possibly-clamped) encoded value rather than the pasted text
// verbatim -- the two can no longer diverge, since both now come out of the
// same encode_guarded call.
TEST(ApplyPaste, ClampsAPastedValueToTheDefinitionMaximum)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "0", "0", "0"};
    const std::vector<std::string_view> rows[] = {{"9999"}};

    MapElementSpec spec = linear_uint8_spec(2, 2);
    spec.min_value = "0";
    spec.max_value = "255";

    const auto patch = apply_paste(rom, spec, /*x_size=*/2, /*y_size=*/2, cells,
                                   {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, rows, 15);

    ASSERT_TRUE(patch.has_value());
    // 9999 clamps to the definition's max_value of 255 instead of truncating
    // into the uint8 storage type (9999 == 0x270F, low byte 0x0F).
    EXPECT_EQ((*patch)[0].display_text, "255");
    EXPECT_EQ((*patch)[0].bytes, (std::vector<std::uint8_t>{0xFF}));
}

// Design notes section 5: legacy sizes its column loop from the FIRST row's
// tab count and indexes every row unconditionally at that width, which is an
// out-of-bounds QStringList::operator[] for a shorter later row. This port
// instead skips a cell whenever the row itself doesn't have a column at that
// position -- defined behavior (silently drop it) rather than legacy's
// undefined one.
TEST(ApplyPaste, SkipsCellsInARaggedRowShorterThanTheFirstRow)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "0", "0", "0"};
    const std::vector<std::string_view> rows[] = {{"11", "22"}, {"33"}};

    const auto patch = apply_paste(rom, linear_uint8_spec(2, 2), /*x_size=*/2, /*y_size=*/2, cells,
                                   {.first_row = 0, .first_col = 0, .last_row = 1, .last_col = 1}, rows, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 3U);
    EXPECT_EQ((*patch)[0].display_text, "11");
    EXPECT_EQ((*patch)[1].display_text, "22");
    EXPECT_EQ((*patch)[2].display_text, "33");
}

} // namespace
} // namespace fastecu::calibration
