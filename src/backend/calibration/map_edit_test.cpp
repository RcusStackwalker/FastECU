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

TEST(ReadRawElement, ReadsFloatRoundTrippedThroughBitCast)
{
    // storagetype == "float" always takes the little_or_float branch
    // regardless of spec.endian (legacy: `endian == "little" || storagetype
    // == "float"`), which fills byte_value from the highest window address
    // down -- equivalent to a little-endian read of the ROM bytes. spec.endian
    // is set to "big" here specifically to demonstrate it is ignored.
    constexpr float kOriginal = 3.14159f;
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(kOriginal);

    auto rom = rom_of(0x20);
    rom[0x10] = static_cast<std::uint8_t>(bits & 0xFF);
    rom[0x11] = static_cast<std::uint8_t>((bits >> 8) & 0xFF);
    rom[0x12] = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    rom[0x13] = static_cast<std::uint8_t>((bits >> 24) & 0xFF);

    const auto value = read_raw_element(rom, spec_for(definition::StorageType::Float, "big"), 0);

    ASSERT_TRUE(value.has_value());
    const auto round_tripped = std::bit_cast<float>(static_cast<std::uint32_t>(*value));
    EXPECT_EQ(round_tripped, kOriginal);
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
