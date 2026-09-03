#include "src/backend/calibration/map_edit.h"

#include <bit>
#include <cstdint>
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
    EXPECT_EQ(static_cast<std::uint32_t>(*value), 0x3FC00000u);
    EXPECT_EQ(std::bit_cast<float>(static_cast<std::uint32_t>(*value)), 1.5f);
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
    EXPECT_EQ(element_byte_address(spec, 0, /*for_write=*/false), 0x100u);
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
    spec.rom_file_size = 180 * 1024;

    EXPECT_NE(element_byte_address(spec, 0, /*for_write=*/false), element_byte_address(spec, 0, /*for_write=*/true));
}

} // namespace
} // namespace fastecu::calibration
