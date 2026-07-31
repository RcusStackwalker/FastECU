#include "src/backend/calibration/calibration_service.h"

#include <format>

#include <gtest/gtest.h>

#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

namespace fastecu::calibration
{
namespace
{

using fastecu::InMemoryFileRepository;
using fastecu::definition::AxisDefinition;
using fastecu::definition::CalibrationMap;
using fastecu::definition::RomDefinition;

TEST(SaveRom, WritesBytesThroughRepository)
{
    InMemoryFileRepository repo;
    const std::vector<std::uint8_t> data{0x01, 0x02, 0x03};

    Status result = save_rom(data, "out.bin", repo);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(repo.files["out.bin"], data);
}

TEST(SaveRom, WriteFailureIsPropagated)
{
    class FailingFileRepository : public InMemoryFileRepository
    {
      public:
        Status write(std::string_view, std::span<const std::uint8_t>) override
        {
            return fastecu::fail(ErrorKind::Internal, "disk full");
        }
    } repo;

    const std::vector<std::uint8_t> data{0x01};
    Status result = save_rom(data, "out.bin", repo);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST(OpenRom, ReadsFromRepositoryWhenNoPreloadedBytes)
{
    InMemoryFileRepository repo;
    repo.files["in.bin"] = {0xAA, 0xBB};

    Result<std::vector<std::uint8_t>> result =
        open_rom("in.bin", {}, "backup.bin", repo);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::uint8_t>{0xAA, 0xBB}));
    EXPECT_TRUE(repo.files.find("backup.bin") == repo.files.end());
}

TEST(OpenRom, ReadFailureIsPropagated)
{
    InMemoryFileRepository repo;

    Result<std::vector<std::uint8_t>> result =
        open_rom("missing.bin", {}, "backup.bin", repo);

    ASSERT_FALSE(result.has_value());
}

TEST(OpenRom, PreloadedBytesAreReturnedUnchangedAndBackedUp)
{
    InMemoryFileRepository repo;
    const std::vector<std::uint8_t> preloaded{0x01, 0x02, 0x03, 0x04};

    Result<std::vector<std::uint8_t>> result =
        open_rom("ignored.bin", preloaded, "backup.bin", repo);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, preloaded);
    EXPECT_EQ(repo.files["backup.bin"], preloaded);
    EXPECT_EQ(repo.read_count("ignored.bin"), 0);
}

TEST(OpenRom, BackupWriteFailureDoesNotFailTheOpen)
{
    class FailingBackupRepository : public InMemoryFileRepository
    {
      public:
        Status write(std::string_view, std::span<const std::uint8_t>) override
        {
            return fastecu::fail(ErrorKind::Internal, "backup failed");
        }
    } repo;
    const std::vector<std::uint8_t> preloaded{0x01};

    Result<std::vector<std::uint8_t>> result =
        open_rom("ignored.bin", preloaded, "backup.bin", repo);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, preloaded);
}

RomDefinition definition_with_one_map(std::optional<std::uint64_t> map_address,
                                      std::optional<std::uint64_t> x_axis_address,
                                      std::optional<std::uint64_t> y_axis_address)
{
    RomDefinition definition;
    CalibrationMap map;
    map.address = map_address;
    map.x_axis.address = x_axis_address;
    map.y_axis.address = y_axis_address;
    definition.maps.push_back(map);
    return definition;
}

TEST(ValidateRomSize, PassesWhenEveryAddressFitsWithinRomLength)
{
    RomDefinition definition = definition_with_one_map(0x1000, 0x100, 0x200);

    EXPECT_TRUE(validate_rom_size(definition, 0x2000).has_value());
}

TEST(ValidateRomSize, FailsWhenMapAddressExceedsRomLength)
{
    RomDefinition definition = definition_with_one_map(0x3000, std::nullopt, std::nullopt);

    Status result = validate_rom_size(definition, 0x2000);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(ValidateRomSize, FailsWhenXAxisAddressExceedsRomLength)
{
    RomDefinition definition = definition_with_one_map(0x100, 0x3000, std::nullopt);

    EXPECT_FALSE(validate_rom_size(definition, 0x2000).has_value());
}

TEST(ValidateRomSize, FailsWhenYAxisAddressExceedsRomLength)
{
    RomDefinition definition = definition_with_one_map(0x100, std::nullopt, 0x3000);

    EXPECT_FALSE(validate_rom_size(definition, 0x2000).has_value());
}

TEST(ValidateRomSize, AddressExactlyAtRomLengthPasses)
{
    // Matches legacy's strict ">" comparison (file_actions.cpp's
    // AddressList.at(i).toUInt() > FullRomData.length()): an address equal
    // to the ROM length is not rejected, even though it points one byte past
    // the end. Preserved verbatim, not "fixed".
    RomDefinition definition = definition_with_one_map(0x2000, std::nullopt, std::nullopt);

    EXPECT_TRUE(validate_rom_size(definition, 0x2000).has_value());
}

TEST(ValidateRomSize, AbsentAddressesDoNotFail)
{
    RomDefinition definition = definition_with_one_map(std::nullopt, std::nullopt, std::nullopt);

    EXPECT_TRUE(validate_rom_size(definition, 0).has_value());
}

TEST(ValidateRomSize, EmptyMapsListPasses)
{
    RomDefinition definition;

    EXPECT_TRUE(validate_rom_size(definition, 0).has_value());
}

TEST(ApplyFlashMethodPadding, InsertsPaddingForMatchingUnderSizedRom)
{
    std::vector<std::uint8_t> rom(160 * 1024, 0x00);

    std::vector<std::uint8_t> result =
        apply_flash_method_padding(std::move(rom), "sub_ecu_denso_mc68hc16y5_02_variant");

    EXPECT_EQ(result.size(), 160 * 1024 + 0x8000);
    // The 0x8000 inserted bytes are all 0xFF, starting at offset 0x20000.
    EXPECT_EQ(result[0x20000], 0xFF);
    EXPECT_EQ(result[0x20000 + 0x7FFF], 0xFF);
}

TEST(ApplyFlashMethodPadding, NoOpWhenFlashMethodDoesNotMatch)
{
    std::vector<std::uint8_t> rom(160 * 1024, 0xAB);

    std::vector<std::uint8_t> result =
        apply_flash_method_padding(rom, "sub_ecu_some_other_method");

    EXPECT_EQ(result, rom);
}

TEST(ApplyFlashMethodPadding, NoOpWhenRomIsAtOrAboveThreshold)
{
    std::vector<std::uint8_t> rom(190 * 1024, 0xAB);

    std::vector<std::uint8_t> result =
        apply_flash_method_padding(rom, "sub_ecu_denso_mc68hc16y5_02");

    EXPECT_EQ(result, rom);
}

TEST(ApplyFlashMethodPadding, GrowsAndZeroFillsRomShorterThanInsertionPoint)
{
    std::vector<std::uint8_t> rom(100, 0xAB);

    std::vector<std::uint8_t> result =
        apply_flash_method_padding(std::move(rom), "sub_ecu_denso_mc68hc16y5_02");

    ASSERT_EQ(result.size(), static_cast<std::size_t>(0x20000) + 0x8000);
    EXPECT_EQ(result[99], 0xAB);
    EXPECT_EQ(result[100], 0x00);     // zero-filled gap
    EXPECT_EQ(result[0x1FFFF], 0x00); // last zero-filled gap byte
    EXPECT_EQ(result[0x20000], 0xFF); // padding starts here
    EXPECT_EQ(result[0x20000 + 0x7FFF], 0xFF);
}

TEST(DecodeScaledValues, DecodesUint16BigEndianWithIdentityExpression)
{
    std::vector<std::uint8_t> rom(200, 0x00);
    rom[100] = 0xAB;
    rom[101] = 0xCD;

    Result<std::string> result = decode_scaled_values(
        rom, /*base_address=*/100, /*count=*/1, /*start_position=*/"1",
        /*interval=*/"1", /*storage_type=*/"uint16", /*endian=*/"big",
        /*from_byte_expression=*/"x", /*is_selectable=*/false,
        /*apply_wrx02_wraparound=*/false, /*float_precision=*/15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "43981,"); // 0xABCD
}

TEST(DecodeScaledValues, GenuinelyLittleEndianReadsFirstByteAsLeastSignificant)
{
    // Fix (disclosed, Global Constraints item 1): legacy's endian=="little"
    // path for non-float types always read a stray 0 regardless of ROM
    // content. This proves the fixed behavior: bytes stored LSB-first (true
    // little-endian) decode to the same numeric value as the big-endian
    // test above, when the byte order in the file is reversed accordingly.
    std::vector<std::uint8_t> rom(200, 0x00);
    rom[100] = 0xCD; // LSB first
    rom[101] = 0xAB; // MSB second

    Result<std::string> result = decode_scaled_values(
        rom, 100, 1, "1", "1", "uint16", "little", "x", false, false, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "43981,"); // same value as the big-endian test, 0xABCD
}

TEST(DecodeScaledValues, DecodesSignedInt16BigEndianNegativeValue)
{
    std::vector<std::uint8_t> rom(200, 0x00);
    rom[100] = 0xFF;
    rom[101] = 0xFE; // 0xFFFE as signed int16 == -2

    Result<std::string> result = decode_scaled_values(
        rom, 100, 1, "1", "1", "int16", "big", "x", false, false, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "-2,");
}

TEST(DecodeScaledValues, SignedAccumulationIsWellDefinedRegardlessOfCharSignedness)
{
    // Fix (disclosed, Global Constraints item 2): a byte with its high bit
    // set (0x80) in a non-final position must not corrupt the accumulated
    // result via intermediate sign extension. 0x80,0x01 as signed int16
    // (big-endian) == -32767.
    std::vector<std::uint8_t> rom(200, 0x00);
    rom[100] = 0x80;
    rom[101] = 0x01;

    Result<std::string> result = decode_scaled_values(
        rom, 100, 1, "1", "1", "int16", "big", "x", false, false, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "-32767,");
}

TEST(DecodeScaledValues, DecodesFloatBigEndianRegardlessOfEndianField)
{
    std::vector<std::uint8_t> rom(200, 0x00);
    // IEEE-754 float32 for 1.5, big-endian byte order: 0x3F,0xC0,0x00,0x00.
    rom[100] = 0x3F;
    rom[101] = 0xC0;
    rom[102] = 0x00;
    rom[103] = 0x00;

    Result<std::string> result = decode_scaled_values(
        rom, 100, 1, "1", "1", "float", "big", "x", false, false, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "1.5,");
}

TEST(DecodeScaledValues, MultipleCellsProduceTrailingCommaAfterEveryValue)
{
    std::vector<std::uint8_t> rom(200, 0x00);
    rom[100] = 0x00;
    rom[101] = 0x01; // uint16 big-endian == 1
    rom[102] = 0x00;
    rom[103] = 0x02; // uint16 big-endian == 2

    Result<std::string> result = decode_scaled_values(
        rom, 100, 2, "1", "1", "uint16", "big", "x", false, false, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "1,2,"); // trailing comma after the LAST value too
}

TEST(DecodeScaledValues, IsSelectableEmitsZeroRegardlessOfBytes)
{
    std::vector<std::uint8_t> rom(200, 0xFF); // non-zero bytes, must be ignored

    Result<std::string> result = decode_scaled_values(
        rom, 100, 1, "1", "1", "uint16", "big", "x", /*is_selectable=*/true,
        false, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "0,");
}

TEST(DecodeScaledValues, StartPositionAndIntervalAreParsedAsHex)
{
    // start_position "10" is hex 0x10 == 16 decimal, not decimal 10.
    // offset = (16 - 1) * storage_size(1) = 15.
    std::vector<std::uint8_t> rom(200, 0x00);
    rom[15] = 0x07;

    Result<std::string> result = decode_scaled_values(
        rom, /*base_address=*/0, 1, /*start_position=*/"10", "1", "uint8",
        "big", "x", false, false, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "7,");
}

TEST(DecodeScaledValues, IntervalHexDigitProvesHexParsingIsActive)
{
    // interval "0A" is hex 0x0A == 10 decimal; "0A" would fail to parse as
    // decimal at all, so a passing test here proves hex parsing is used.
    std::vector<std::uint8_t> rom(200, 0x00);
    rom[0] = 0x01;
    rom[10] = 0x02;

    Result<std::string> result = decode_scaled_values(
        rom, 0, 2, "1", "0A", "uint8", "big", "x", false, false, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "1,2,");
}

TEST(DecodeScaledValues, EmptyStartPositionUnderflowsToASafeError)
{
    // Legacy: (startPos - 1) on an unsigned 32-bit value with startPos == 0
    // (empty/unparseable start_position) underflows to 0xFFFFFFFF, sending
    // the computed address far out of bounds. Reproduced exactly (not
    // "fixed" to avoid the underflow) -- the bounds check below it turns
    // the resulting huge address into a clean error rather than a crash.
    std::vector<std::uint8_t> rom(200, 0x00);

    Result<std::string> result = decode_scaled_values(
        rom, 0, 1, /*start_position=*/"", "1", "uint8", "big", "x", false,
        false, 15);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST(DecodeScaledValues, FailsWhenByteAddressExceedsRomSize)
{
    std::vector<std::uint8_t> rom(10, 0x00);

    Result<std::string> result = decode_scaled_values(
        rom, /*base_address=*/50, 1, "1", "1", "uint8", "big", "x", false,
        false, 15);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST(DecodeScaledValues, Wrx02WraparoundBringsOutOfBoundsAddressIntoRange)
{
    std::vector<std::uint8_t> rom(100, 0x00);
    rom[16] = 0x2A; // 42

    Result<std::string> result = decode_scaled_values(
        rom, /*base_address=*/0x8010, 1, "1", "1", "uint8", "big", "x",
        false, /*apply_wrx02_wraparound=*/true, 15);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "42,"); // 0x8010 - 0x8000 == 16
}

TEST(DecodeScaledValues, WithoutWraparoundSameAddressFails)
{
    std::vector<std::uint8_t> rom(100, 0x00);

    Result<std::string> result = decode_scaled_values(
        rom, 0x8010, 1, "1", "1", "uint8", "big", "x", false,
        /*apply_wrx02_wraparound=*/false, 15);

    ASSERT_FALSE(result.has_value());
}

TEST(DecodeScaledValues, FormattingMatchesCapturedQtGroundTruth)
{
    // Ground truth captured from real QString::number(value, 'g', 15) --
    // see the design doc's "Named Risks" section for the full table and how
    // it was captured. from_byte_expression is set to a bare numeric
    // literal (std::format("{:.20f}", c.value)), not an expression
    // referencing the decoded byte -- expression_evaluate's tokenizer
    // (verified by reading src/algorithms/expression/expression_evaluator.cpp)
    // only special-cases the literal character 'x'; any other token falls
    // through to std::stod, so a pure-literal expression evaluates to
    // exactly that literal's double value with no risk of the
    // floating-point cancellation error an arithmetic trick like "x - 1 +
    // 1" would introduce for small magnitudes (e.g. 0.00001). This
    // isolates the *formatting* function (decode_scaled_values's
    // format_like_qt_g) from the byte-decode logic, already covered by
    // the tests above -- the single decoded ROM byte is irrelevant here
    // since the expression never references x.
    //
    // Fixed-point ("f") formatting, not std::to_string or "g"/"e" style,
    // and specifically not scientific notation: std::to_string(double) is
    // fixed 6-decimal-place notation (equivalent to printf "%f") and
    // silently truncates/rounds values needing more precision or a smaller
    // magnitude than that -- e.g. std::to_string(3.14159265358979) yields
    // "3.141593" and std::to_string(0.000000001) yields "0.000000". A "g"
    // or "e" style format (e.g. "{:.17g}") avoids that truncation but
    // introduces a different corruption: for small magnitudes it emits
    // scientific notation ("1.0000000000000001e-05"), and
    // expression_parse's hand-rolled tokenizer has no exponent support --
    // it silently drops the 'e' as an unrecognized character and then
    // misreads the following "-05" as a subtraction operator applied to
    // the mantissa, corrupting the value entirely (confirmed empirically:
    // "{:.17g}" on 0.00001 broke a case that std::to_string had actually
    // gotten right). "{:.20f}" (fixed-point, 20 digits after the decimal
    // point, never scientific notation) round-trips every value in this
    // table exactly through std::stod, verified with a standalone probe.
    struct Case
    {
        double value;
        const char *expected;
    };
    const Case cases[] = {
        {0.0, "0"},
        {-0.0, "0"},
        {1.0, "1"},
        {123.0, "123"},
        {123.456, "123.456"},
        {-45.6, "-45.6"},
        {0.0001, "0.0001"},
        {0.00001, "1e-05"},
        {100000.0, "100000"},
        {123456789012345.0, "123456789012345"},
        {3.14159265358979, "3.14159265358979"},
        {0.000000001, "1e-09"},
    };
    std::vector<std::uint8_t> rom(4, 0x01);
    for (const Case& c : cases)
    {
        Result<std::string> result = decode_scaled_values(
            rom, 0, 1, "1", "1", "uint8", "big", std::format("{:.20f}", c.value),
            false, false, 15);
        ASSERT_TRUE(result.has_value()) << c.expected;
        EXPECT_EQ(*result, std::string(c.expected) + ",") << "value=" << c.value;
    }
}

} // namespace
} // namespace fastecu::calibration
