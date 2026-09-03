#include "src/backend/calibration/map_edit.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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

// get_rom_data_value reads a signed multi-byte value back out of a union
// whose int8_t/int16_t/int32_t members alias the same storage as the
// uint8_t byte_value[4] array that assembly filled MOST-significant-byte-
// first in BOTH endian branches (see map_edit.cpp's read_raw_element). On a
// little-endian host, the union's *_value[0] member reads byte_value[0] as
// the LEAST significant byte -- the opposite of how it was filled -- which
// byte-swaps every signed multi-byte read relative to what the endian field
// says. Confirmed by running this port: for big-endian ROM bytes 0x01 0x02,
// a correct big-endian read would be 0x0102 (258); this reads 0x0201 (513)
// instead. Ported verbatim; spec's defect (b). Task 6b-4 fixes this.
TEST(ReadRawElement, PinnedDefect_SignedMultiByteReadsAreByteSwapped_Int16Big)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int16, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x0201);
}

TEST(ReadRawElement, PinnedDefect_SignedMultiByteReadsAreByteSwapped_Int16Little)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int16, "little"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x0102);
}

TEST(ReadRawElement, PinnedDefect_SignedMultiByteReadsAreByteSwapped_Int32Big)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;
    rom[0x13] = 0x04;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int32, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x04030201);
}

TEST(ReadRawElement, PinnedDefect_SignedMultiByteReadsAreByteSwapped_Int32Little)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;
    rom[0x13] = 0x04;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int32, "little"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x01020304);
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

// Legacy's signed branch tests only storagesize 1, 2, and 4 -- a 3-byte
// signed value falls through every `if`, so `value` (a QString) is never
// assigned and stays empty; callers convert an empty QString to 0. This is
// legacy behavior being pinned, not an oversight in this port -- spec's
// defect (f). Non-zero ROM bytes are used deliberately, to show the 0 is not
// simply an artifact of an all-zero fixture.
TEST(ReadRawElement, PinnedDefect_Int24AlwaysReadsAsZero)
{
    auto rom = rom_of(0x20);
    rom[0x10] = 0x01;
    rom[0x11] = 0x02;
    rom[0x12] = 0x03;

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Int24, "big"), 0);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0);
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
// same element. Ported verbatim; spec's defect (a). Task 6b-4 reconciles them.
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
// Only the cases that actually round-trip live here: every unsigned width at
// both endians, and the one signed width (1 byte) where read_raw_element's
// byte-swap defect (see PinnedDefect_SignedMultiByteReadsAreByteSwapped_*
// above) has no width to swap within. The signed multi-byte cases that do NOT
// round-trip are pinned separately below, as
// PinnedDefect_SignedMultiByteDoesNotRoundTripBecauseTheReadIsByteSwapped --
// keeping them here as EXPECT_EQ would leave this suite red forever, which
// defeats it as a regression net for every case that DOES round-trip today.
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
        {definition::StorageType::Uint16, "little", 0x1234},     {definition::StorageType::Uint32, "big", 0x12345678},
        {definition::StorageType::Uint32, "little", 0x12345678}, {definition::StorageType::Int8, "big", -2},
    };

    for (const auto& c : cases)
    {
        MapElementSpec spec;
        spec.address = 0x10;
        spec.storage_type = c.storage;
        spec.endian = c.endian;
        spec.to_byte = "x";
        spec.from_byte = "x";

        const auto encoded = encode_scaled_value(spec, double(c.raw), 15, /*legacy_byte_order=*/false);
        ASSERT_TRUE(encoded.has_value()) << to_string(encoded.error().kind);

        std::vector<std::uint8_t> rom(0x40, 0x00);
        std::ranges::copy(*encoded, rom.begin() + 0x10);

        const auto decoded = read_raw_element(rom, spec, 0);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, c.raw) << "storage=" << definition::storage_type_text(c.storage) << " endian=" << c.endian;
    }
}

// Pinned, not fixed: encode_scaled_value writes the byte order spec.endian's
// label claims (matching decode_scaled_values, the correct decoder). But
// read_raw_element -- reproducing get_rom_data_value verbatim, per Task 2's
// PinnedDefect_SignedMultiByteReadsAreByteSwapped_* -- assembles signed
// multi-byte values MSB-first regardless of endian, which is backwards from
// how it filled little-endian reads. Reading back what encode_scaled_value
// just wrote therefore recovers a different number than what was encoded.
// This is a real, live hazard for the eventual edit-apply path (6b-4's
// scope), not a test artifact: an edit UI that writes int16/int32 values and
// then re-reads the cell via read_raw_element to redraw it would show the
// wrong number immediately after a successful write.
//
// Each case's "should be" value in the comment is the raw value that was
// encoded; "is" is what read_raw_element actually returns after decoding
// encode_scaled_value's bytes, computed by hand from the two functions'
// documented algorithms and confirmed by this test.
TEST(EncodeScaledValue, PinnedDefect_SignedMultiByteDoesNotRoundTripBecauseTheReadIsByteSwapped)
{
    struct Case
    {
        definition::StorageType storage;
        std::string_view endian;
        std::int64_t raw;
        std::int64_t decoded_instead;
    };
    const Case cases[] = {
        // int16 big, encoded [0xFE, 0xD4]: read_raw_element reassembles
        // byte_value[0]=0xFE, byte_value[1]=0xD4 -> raw16 0xD4FE -> sign
        // extended -11010. Should be -300.
        {definition::StorageType::Int16, "big", -300, -11010},
        // int16 little, encoded [0xD4, 0xFE]: read_raw_element's little-endian
        // fill reverses it back to byte_value[0]=0xFE, byte_value[1]=0xD4 --
        // the same swapped pair as the big-endian case above -- so it lands
        // on the same wrong value, -11010. Should be -300.
        {definition::StorageType::Int16, "little", -300, -11010},
        // int32 big, encoded [0xFF, 0xFE, 0xEE, 0x90]: read_raw_element
        // assembles raw32 = 0x90EEFEFF (LSB-first from byte_value[0..3] =
        // the MSB-first-written bytes) -> sign extended -1863385345. Should
        // be -70000.
        {definition::StorageType::Int32, "big", -70000, -1863385345},
    };

    for (const auto& c : cases)
    {
        MapElementSpec spec;
        spec.address = 0x10;
        spec.storage_type = c.storage;
        spec.endian = c.endian;
        spec.to_byte = "x";
        spec.from_byte = "x";

        const auto encoded = encode_scaled_value(spec, double(c.raw), 15, /*legacy_byte_order=*/false);
        ASSERT_TRUE(encoded.has_value()) << to_string(encoded.error().kind);

        std::vector<std::uint8_t> rom(0x40, 0x00);
        std::ranges::copy(*encoded, rom.begin() + 0x10);

        const auto decoded = read_raw_element(rom, spec, 0);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_NE(*decoded, c.raw) << "storage=" << definition::storage_type_text(c.storage) << " endian=" << c.endian
                                   << " -- if this now passes, the byte-swap"
                                   << " defect this test pins was fixed elsewhere; update/remove this test"
                                   << " rather than leaving a misleading EXPECT_NE.";
        EXPECT_EQ(*decoded, c.decoded_instead)
            << "storage=" << definition::storage_type_text(c.storage) << " endian=" << c.endian;
    }
}

// set_rom_data_value packs the raw value into a host-native little-endian
// buffer (via a union whose bit pattern set_rom_data_value's callers arrange
// to already equal the encoded raw value -- see menu_actions.cpp:335-346's
// `map_data_value.dword_value = new_rom_data_value.toUInt()` followed by
// passing `map_data_value.float_value` through the float parameter, which
// round-trips the same bits unchanged) and then indexes it as shown below.
//
// Measured: for uint16 "big" raw 0x1234, encode_scaled_value writes
// [0x12, 0x34] (MSB-first, matching the "big" label). Legacy's loop -- with
// endian == "big", so the `else` branch, `byte_value[k]` in host order --
// writes [0x34, 0x12] instead: host_bytes for the little-endian host is
// [0x34, 0x12, 0x00, 0x00], and legacy[k] = host_bytes[k] for k in {0,1}.
// That is the little-endian byte order despite the "big" label. Legacy's
// write path is therefore ALSO inverted relative to its endian labels --
// consistent with (and structurally the same defect as) the read-side
// byte-swap Task 2 pinned. This is recorded here as a divergence, not
// silently reconciled: 6b-4 is where the fix (if any) belongs, informed by
// corpus evidence of which behavior real EcuFlash defs actually rely on.
TEST(EncodeScaledValue, PinnedDefect_WriteOrderDivergesFromLegacyBecauseLegacyIsAlsoByteSwapped)
{
    MapElementSpec spec;
    spec.storage_type = definition::StorageType::Uint16;
    spec.endian = "big";
    spec.to_byte = "x";

    const auto encoded = encode_scaled_value(spec, 0x1234, 15, /*legacy_byte_order=*/false);
    ASSERT_TRUE(encoded.has_value());

    const auto host_bytes = std::bit_cast<std::array<std::uint8_t, 4>>(std::uint32_t{0x1234});
    std::vector<std::uint8_t> legacy(2);
    for (std::uint32_t k = 0; k < 2; ++k)
    {
        legacy[k] = (spec.endian == "little") ? host_bytes[2 - 1 - k] : host_bytes[k];
    }

    // Measured: encoded == {0x12, 0x34}, legacy == {0x34, 0x12}.
    EXPECT_NE(*encoded, legacy);
    const std::vector<std::uint8_t> expected_encoded{0x12, 0x34};
    const std::vector<std::uint8_t> expected_legacy{0x34, 0x12};
    EXPECT_EQ(*encoded, expected_encoded);
    EXPECT_EQ(legacy, expected_legacy);
}

// legacy_byte_order selects between the two measured write orders directly:
// false reproduces the correct, label-matching order; true reproduces
// legacy set_rom_data_value's order, which is inverted relative to its own
// endian label (raw 0x1234 labeled "big" writes [0x34, 0x12] -- the same
// measurement as PinnedDefect_WriteOrderDivergesFromLegacyBecauseLegacyIsAlsoByteSwapped
// above, now asserted directly through the flag rather than reconstructed by
// hand). Float storage is unaffected by the flag in either mode: floats are
// always written big-endian-in-ROM.
TEST(EncodeScaledValue, LegacyByteOrderFlagSelectsWriteOrder)
{
    MapElementSpec spec;
    spec.storage_type = definition::StorageType::Uint16;
    spec.endian = "big";
    spec.to_byte = "x";

    const auto correct_order = encode_scaled_value(spec, 0x1234, 15, /*legacy_byte_order=*/false);
    ASSERT_TRUE(correct_order.has_value());
    EXPECT_EQ(*correct_order, (std::vector<std::uint8_t>{0x12, 0x34}));

    const auto legacy_order = encode_scaled_value(spec, 0x1234, 15, /*legacy_byte_order=*/true);
    ASSERT_TRUE(legacy_order.has_value());
    EXPECT_EQ(*legacy_order, (std::vector<std::uint8_t>{0x34, 0x12}));

    MapElementSpec float_spec;
    float_spec.storage_type = definition::StorageType::Float;
    float_spec.endian = "big";
    float_spec.to_byte = "x";

    const auto float_correct = encode_scaled_value(float_spec, 1.5, 15, /*legacy_byte_order=*/false);
    ASSERT_TRUE(float_correct.has_value());
    const auto float_legacy = encode_scaled_value(float_spec, 1.5, 15, /*legacy_byte_order=*/true);
    ASSERT_TRUE(float_legacy.has_value());
    EXPECT_EQ(*float_correct, *float_legacy);
}

// write_raw_element is the byte-packing half of encode_scaled_value, and is
// itself the faithful port of legacy set_rom_data_value -- a pure byte
// writer with no expression evaluation and no rounding of its own. This
// reproduces legacy's packing loop independently (byte_value[] as the
// little-endian-host decomposition of the raw dword, indexed the same way
// set_rom_data_value's loop does -- the same technique
// PinnedDefect_WriteOrderDivergesFromLegacyBecauseLegacyIsAlsoByteSwapped
// uses above) for every width read_raw_element handles, both non-float
// endian labels, and the float bit-pattern case, and asserts
// write_raw_element(..., /*legacy_byte_order=*/true) matches it exactly.
TEST(WriteRawElement, MatchesLegacySetRomDataValueLoopForEveryWidth)
{
    struct Case
    {
        definition::StorageType storage;
        std::string_view endian;
        std::int64_t raw;
    };
    const Case cases[] = {
        {definition::StorageType::Uint8, "big", 0xAB},
        {definition::StorageType::Uint8, "little", 0xAB},
        {definition::StorageType::Uint16, "big", 0x1234},
        {definition::StorageType::Uint16, "little", 0x1234},
        {definition::StorageType::Uint24, "big", 0x123456},
        {definition::StorageType::Uint24, "little", 0x123456},
        {definition::StorageType::Uint32, "big", 0x12345678},
        {definition::StorageType::Uint32, "little", 0x12345678},
        {definition::StorageType::Int8, "big", -2},
        {definition::StorageType::Int16, "big", -300},
        {definition::StorageType::Int32, "big", -70000},
        // Float: raw is a BIT PATTERN, not a number to convert -- the encoded
        // bits of 1.5F, exactly what encode_scaled_value's bit_cast produces
        // and what legacy's callers smuggle through the float parameter.
        {definition::StorageType::Float, "big", static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(1.5F))},
    };

    for (const auto& c : cases)
    {
        MapElementSpec spec;
        spec.storage_type = c.storage;
        spec.endian = c.endian;

        const auto written = write_raw_element(spec, c.raw, /*legacy_byte_order=*/true);
        ASSERT_TRUE(written.has_value()) << to_string(written.error().kind);

        const std::uint32_t width = definition::storage_byte_size(c.storage);
        const bool is_float = c.storage == definition::StorageType::Float;
        const auto host_bytes = std::bit_cast<std::array<std::uint8_t, 4>>(static_cast<std::uint32_t>(c.raw));
        std::vector<std::uint8_t> legacy(width);
        for (std::uint32_t k = 0; k < width; ++k)
        {
            legacy[k] = (c.endian == "little" || is_float) ? host_bytes[width - 1 - k] : host_bytes[k];
        }

        EXPECT_EQ(*written, legacy) << "storage=" << definition::storage_type_text(c.storage) << " endian=" << c.endian;
    }
}

// Direct write_raw_element counterpart of EncodeScaledValue's
// LegacyByteOrderFlagSelectsWriteOrder above, confirming the flag's meaning
// survives the encode_scaled_value/write_raw_element split unchanged.
TEST(WriteRawElement, LegacyByteOrderFlagSelectsWriteOrder)
{
    MapElementSpec spec;
    spec.storage_type = definition::StorageType::Uint16;
    spec.endian = "big";

    const auto correct_order = write_raw_element(spec, 0x1234, /*legacy_byte_order=*/false);
    ASSERT_TRUE(correct_order.has_value());
    EXPECT_EQ(*correct_order, (std::vector<std::uint8_t>{0x12, 0x34}));

    const auto legacy_order = write_raw_element(spec, 0x1234, /*legacy_byte_order=*/true);
    ASSERT_TRUE(legacy_order.has_value());
    EXPECT_EQ(*legacy_order, (std::vector<std::uint8_t>{0x34, 0x12}));

    MapElementSpec float_spec;
    float_spec.storage_type = definition::StorageType::Float;
    float_spec.endian = "big";
    const std::int64_t float_bits = static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(1.5F));

    const auto float_correct = write_raw_element(float_spec, float_bits, /*legacy_byte_order=*/false);
    ASSERT_TRUE(float_correct.has_value());
    const auto float_legacy = write_raw_element(float_spec, float_bits, /*legacy_byte_order=*/true);
    ASSERT_TRUE(float_legacy.has_value());
    EXPECT_EQ(*float_correct, *float_legacy);
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

TEST(MapCellColorScale, MapsTheMinimumToTheTopOfTheHueRange)
{
    // scale_start is 210/360; the legacy formula sends min -> 0 and max ->
    // scale_start.
    EXPECT_DOUBLE_EQ(map_cell_color_scale(0.0, 0.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(map_cell_color_scale(100.0, 0.0, 100.0), 210.0 / 360.0);
}

TEST(MapCellColorScale, ClampsBelowTheMinimumToZero)
{
    EXPECT_DOUBLE_EQ(map_cell_color_scale(-50.0, 0.0, 100.0), 0.0);
}

// Display-only defect: MapCellColorMin == MapCellColorMax (a definition
// coloring a map with a flat range) divides by zero in the legacy formula.
// value == min_value == max_value makes both the numerator and denominator
// zero, so every step operates on 0.0/0.0 (NaN) rather than a signed
// infinity -- unambiguously non-finite regardless of the sign of
// scale_start. This is presentation-only (no ROM write involved) and does
// not join the write-path defects the spec tracks for the 6b-4 fix wave.
TEST(MapCellColorScale, PinnedDefect_EqualColorBoundsProduceNonFiniteHue)
{
    const double result = map_cell_color_scale(50.0, 50.0, 50.0);

    EXPECT_TRUE(std::isnan(result));
}

} // namespace
} // namespace fastecu::calibration
