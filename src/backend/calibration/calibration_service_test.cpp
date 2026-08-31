#include "src/backend/calibration/calibration_service.h"

#include <format>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "src/backend/ports/testing/in_memory_file_repository.h"

namespace fastecu::calibration
{
namespace
{

using fastecu::InMemoryFileRepository;
using fastecu::definition::AxisDefinition;
using fastecu::definition::CalibrationMap;
using fastecu::definition::RomDefinition;
using fastecu::definition::Scaling;
using fastecu::definition::StorageType;

TEST(ReadRom, ReadsRequestedHandleThroughRepository)
{
    InMemoryFileRepository repo;
    repo.files["in.bin"] = {0xAA, 0xBB};

    Result<std::vector<std::uint8_t>> result = read_rom("in.bin", repo);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::uint8_t>{0xAA, 0xBB}));
}

TEST(ReadRom, ReadFailureIsPropagated)
{
    InMemoryFileRepository repo;

    Result<std::vector<std::uint8_t>> result = read_rom("missing.bin", repo);

    ASSERT_FALSE(result.has_value());
}

TEST(ReadRom, EmptyRomIsAValidResultNotAMissingOne)
{
    // The two-mode open_rom this replaced used span emptiness to pick its
    // mode, which conflated "no preloaded bytes" with "a zero-length ROM".
    // read_rom has one mode, so a zero-length file is just a zero-length
    // successful read.
    InMemoryFileRepository repo;
    repo.files["empty.bin"] = {};

    Result<std::vector<std::uint8_t>> result = read_rom("empty.bin", repo);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(BackupRom, WritesBytesToBackupHandle)
{
    InMemoryFileRepository repo;
    const std::vector<std::uint8_t> rom_data{0x01, 0x02, 0x03, 0x04};

    backup_rom(rom_data, "backup.bin", repo);

    EXPECT_EQ(repo.files["backup.bin"], rom_data);
    EXPECT_EQ(repo.read_count("backup.bin"), 0);
}

TEST(BackupRom, WriteFailureIsSwallowed)
{
    // The whole reason backup_rom returns void: open_subaru_rom_file never
    // inspected this write's result, so a failed backup must not be able to
    // fail the open.
    class FailingBackupRepository : public InMemoryFileRepository
    {
      public:
        Status write(std::string_view, std::span<const std::uint8_t>) override
        {
            return fastecu::fail(ErrorKind::Internal, "backup failed");
        }
    } repo;
    const std::vector<std::uint8_t> rom_data{0x01};

    backup_rom(rom_data, "backup.bin", repo);

    EXPECT_TRUE(repo.files.find("backup.bin") == repo.files.end());
}

TEST(ElementByteSizeTest, DelegatesToStorageByteSizeWhenNotBloblist)
{
    EXPECT_EQ(element_byte_size(StorageType::Uint16, nullptr), 2U);
    EXPECT_EQ(element_byte_size(std::optional<StorageType>{}, nullptr), 1U);
}

TEST(ElementByteSizeTest, BloblistWithNoScalingFallsBackToOneByte)
{
    EXPECT_EQ(element_byte_size(StorageType::Bloblist, nullptr), 1U);
}

TEST(ElementByteSizeTest, BloblistWithEmptySelectionsFallsBackToOneByte)
{
    Scaling scaling;
    EXPECT_EQ(element_byte_size(StorageType::Bloblist, &scaling), 1U);
}

TEST(ElementByteSizeTest, BloblistWidthComesFromFirstSelectionHexLength)
{
    Scaling scaling;
    scaling.selections = {{"disabled", "0000"}, {"enabled", "0001"}};

    EXPECT_EQ(element_byte_size(StorageType::Bloblist, &scaling), 2U);
}

TEST(ElementByteSizeTest, BloblistWidthMatchesLegacySingleByteSelections)
{
    Scaling scaling;
    scaling.selections = {{"disabled", "00"}, {"enabled", "01"}};

    EXPECT_EQ(element_byte_size(StorageType::Bloblist, &scaling), 1U);
}

TEST(ElementRunEndTest, SingleElementIsAddressPlusWidth)
{
    EXPECT_EQ(element_run_end(0x1000, /*start_position=*/1, /*interval=*/1,
                              /*element_width=*/4, /*count=*/1),
              0x1004U);
}

TEST(ElementRunEndTest, ContiguousRunMatchesCountTimesWidth)
{
    // address=0x100, start_position=1, interval=1, width=2, count=4:
    // elements at 0x100, 0x102, 0x104, 0x106; last occupies [0x106, 0x108).
    EXPECT_EQ(element_run_end(0x100, 1, 1, 2, 4), 0x108U);
}

TEST(ElementRunEndTest, StridedRunMatchesLegacyPerCellAddressFormula)
{
    // address=0x200, start_position=3, interval=2, width=1, count=3.
    // Legacy: addr(j) = address + (start_position-1)*width + j*width*interval.
    // addr(0)=0x202, addr(1)=0x204, addr(2)=0x206; last occupies [0x206, 0x207).
    EXPECT_EQ(element_run_end(0x200, 3, 2, 1, 3), 0x207U);
}

TEST(ElementRunEndTest, ZeroStartPositionIsTreatedAsTheFirstPosition)
{
    // start_position is 1-based and unvalidated upstream, so startpos="0"
    // reaches here. Computing 0 - 1 in uint32 would wrap to 0xFFFFFFFF and
    // put the run ~4 GB past `address`.
    EXPECT_EQ(element_run_end(0x1000, /*start_position=*/0, /*interval=*/1,
                              /*element_width=*/4, /*count=*/1),
              element_run_end(0x1000, 1, 1, 4, 1));
    EXPECT_EQ(element_run_end(0x1000, 0, 1, 4, 1), 0x1004U);
}

TEST(ElementRunEndTest, ZeroStartPositionDoesNotWrapWithAMultiElementRun)
{
    EXPECT_EQ(element_run_end(0x100, /*start_position=*/0, /*interval=*/2,
                              /*element_width=*/2, /*count=*/3),
              0x10AU);
}

TEST(ElementRunEndTest, ZeroCountTouchesNothingPastTheAddress)
{
    // count - 1 would wrap the same way. An empty run ends where it starts.
    EXPECT_EQ(element_run_end(0x1000, /*start_position=*/1, /*interval=*/1,
                              /*element_width=*/4, /*count=*/0),
              0x1000U);
    EXPECT_EQ(element_run_end(0x1000, /*start_position=*/8, /*interval=*/3,
                              /*element_width=*/4, /*count=*/0),
              0x1000U);
}

TEST(ElementRunEndTest, ZeroCountAndZeroStartPositionTogetherDoNotWrap)
{
    EXPECT_EQ(element_run_end(0x1000, 0, 1, 4, 0), 0x1000U);
}

RomDefinition definition_with_one_map(CalibrationMap map, std::vector<Scaling> scalings = {})
{
    RomDefinition definition;
    definition.maps.push_back(std::move(map));
    definition.scalings = std::move(scalings);
    return definition;
}

TEST(ValidateRomSize, PassesWhenEveryAddressFitsWithinRomLength)
{
    CalibrationMap map;
    map.address = 0x1000;
    map.x_axis.address = 0x100;
    map.y_axis.address = 0x200;

    EXPECT_TRUE(validate_rom_size(definition_with_one_map(map), 0x2000).has_value());
}

TEST(ValidateRomSize, FailsWhenMapAddressExceedsRomLength)
{
    CalibrationMap map;
    map.address = 0x3000;

    Status result = validate_rom_size(definition_with_one_map(map), 0x2000);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(ValidateRomSize, FailsWhenXAxisAddressExceedsRomLength)
{
    CalibrationMap map;
    map.address = 0x100;
    map.x_axis.address = 0x3000;

    EXPECT_FALSE(validate_rom_size(definition_with_one_map(map), 0x2000).has_value());
}

TEST(ValidateRomSize, FailsWhenYAxisAddressExceedsRomLength)
{
    CalibrationMap map;
    map.address = 0x100;
    map.y_axis.address = 0x3000;

    EXPECT_FALSE(validate_rom_size(definition_with_one_map(map), 0x2000).has_value());
}

TEST(ValidateRomSize, AddressExactlyAtRomLengthFails)
{
    // A default 1x1, 1-byte-wide map's single byte would be the first byte
    // past the end of a rom_byte_length-sized buffer -- must fail, not pass.
    CalibrationMap map;
    map.address = 0x2000;

    EXPECT_FALSE(validate_rom_size(definition_with_one_map(map), 0x2000).has_value());
}

TEST(ValidateRomSize, AbsentAddressesDoNotFail)
{
    CalibrationMap map;

    EXPECT_TRUE(validate_rom_size(definition_with_one_map(map), 0).has_value());
}

TEST(ValidateRomSize, EmptyMapsListPasses)
{
    RomDefinition definition;

    EXPECT_TRUE(validate_rom_size(definition, 0).has_value());
}

TEST(ValidateRomSize, FailsWhenMapExtentOverflowsWithContiguousStride)
{
    // Base address (0x1000) alone would fit under a base-address-only check,
    // but 4 uint16 elements laid out contiguously (interval=1) end at 0x1008
    // -- past a 0x1005-byte ROM.
    CalibrationMap map;
    map.address = 0x1000;
    map.x_size = 4;
    map.y_size = 1;
    map.storage_type = StorageType::Uint16;
    map.start_position = 1;
    map.interval = 1;

    EXPECT_FALSE(validate_rom_size(definition_with_one_map(map), 0x1005).has_value());
}

TEST(ValidateRomSize, FailsWhenMapExtentOverflowsWithNonContiguousStride)
{
    // 2 uint8 elements with interval=5 span address+5+1=0x1006 -- past a
    // 0x1004-byte ROM, even though a stride-naive count*width computation
    // (0x1000 + 2*1 = 0x1002) would have wrongly passed.
    CalibrationMap map;
    map.address = 0x1000;
    map.x_size = 2;
    map.y_size = 1;
    map.storage_type = StorageType::Uint8;
    map.start_position = 1;
    map.interval = 5;

    EXPECT_FALSE(validate_rom_size(definition_with_one_map(map), 0x1004).has_value());
}

TEST(ValidateRomSize, FailsWhenAxisExtentOverflowsWithNonContiguousStride)
{
    CalibrationMap map;
    map.address = 0x100;
    map.x_axis.address = 0x1000;
    map.x_axis.size = 2;
    map.x_axis.storage_type = StorageType::Uint8;
    map.x_axis.start_position = 1;
    map.x_axis.interval = 5;

    EXPECT_FALSE(validate_rom_size(definition_with_one_map(map), 0x1004).has_value());
}

TEST(ValidateRomSize, BloblistExtentUsesWidthDerivedFromSelections)
{
    // The scaling's selections are 2-byte hex values ("0000"/"0001"), so the
    // map's single element occupies 2 bytes, not the 1-byte fallback -- pushes
    // the extent one byte past a ROM that a naive 1-byte assumption would pass.
    CalibrationMap map;
    map.address = 0x0FFF;
    map.scaling_name = "mode";
    map.storage_type = StorageType::Bloblist;

    Scaling scaling;
    scaling.name = "mode";
    scaling.selections = {{"disabled", "0000"}, {"enabled", "0001"}};

    EXPECT_FALSE(validate_rom_size(definition_with_one_map(map, {scaling}), 0x1000).has_value());
}

TEST(ValidateRomSize, ZeroStartPositionDoesNotSpuriouslyRejectTheRom)
{
    // A definition carrying startpos="0" used to underflow to a ~4 GB extent
    // here, failing the whole ROM ("Error in expected ROM size!") and
    // clearing every one of its maps.
    CalibrationMap map;
    map.address = 0x1000;
    map.x_size = 4;
    map.y_size = 1;
    map.storage_type = StorageType::Uint16;
    map.start_position = 0;
    map.interval = 1;

    EXPECT_TRUE(validate_rom_size(definition_with_one_map(map), 0x2000).has_value());
}

TEST(ValidateRomSize, NullStorageTypeStillGetsExtentCheckedWithOneByteDefault)
{
    // No storage_type anywhere (map's own gap flagged in definition_model.h) --
    // still defaults to 1 byte/element and is checked, not skipped.
    CalibrationMap map;
    map.address = 0x0FFF;
    map.x_size = 2;
    map.y_size = 1;

    EXPECT_FALSE(validate_rom_size(definition_with_one_map(map), 0x1000).has_value());
}

TEST(ApplyFlashMethodPadding, InsertsPaddingForMatchingShortRom)
{
    std::vector<std::uint8_t> rom(0x2A000, 0xAA);
    const std::size_t original = rom.size();

    rom = apply_flash_method_padding(std::move(rom), "sub_ecu_denso_mc68hc16y5_02");

    EXPECT_EQ(rom.size(), original + 0x8000);
    // Bytes before the insertion point are untouched.
    EXPECT_EQ(rom.at(0x1FFFF), 0xAA);
    // The inserted run is 0xFF.
    EXPECT_EQ(rom.at(0x20000), 0xFF);
    EXPECT_EQ(rom.at(0x27FFF), 0xFF);
    // The original byte that was at 0x20000 has moved up by 0x8000.
    EXPECT_EQ(rom.at(0x28000), 0xAA);
}

TEST(ApplyFlashMethodPadding, MatchesOnPrefixSoEcutekVariantAlsoPads)
{
    std::vector<std::uint8_t> rom(0x2A000, 0xAA);
    rom = apply_flash_method_padding(std::move(rom), "sub_ecu_denso_mc68hc16y5_02_ecutek");
    EXPECT_EQ(rom.size(), 0x2A000U + 0x8000U);
}

TEST(ApplyFlashMethodPadding, LeavesOtherFlashMethodsAlone)
{
    std::vector<std::uint8_t> rom(0x30000, 0xAA);
    rom = apply_flash_method_padding(std::move(rom), "sub_ecu_denso_sh7058");
    EXPECT_EQ(rom.size(), 0x30000U);
}

TEST(ApplyFlashMethodPadding, LeavesRomsAtOrAboveTheSizeThresholdAlone)
{
    // 190 * 1024 == 0x2F800; the guard is "< 190 * 1024", so exactly at the
    // threshold must not pad.
    std::vector<std::uint8_t> rom(190UZ * 1024, 0xAA);
    rom = apply_flash_method_padding(std::move(rom), "sub_ecu_denso_mc68hc16y5_02");
    EXPECT_EQ(rom.size(), static_cast<std::size_t>(190 * 1024));
}

TEST(ApplyFlashMethodPadding, ZeroExtendsRomShorterThanTheInsertionPoint)
{
    std::vector<std::uint8_t> rom(0x100, 0xAA);
    rom = apply_flash_method_padding(std::move(rom), "sub_ecu_denso_mc68hc16y5_02");

    EXPECT_EQ(rom.size(), 0x20000U + 0x8000U);
    EXPECT_EQ(rom.at(0xFF), 0xAA);
    // The synthetic gap is zero-filled: Qt's insert leaves it "uninitialized"
    // per its docs, so this is a deliberate deterministic choice.
    EXPECT_EQ(rom.at(0x100), 0x00);
    EXPECT_EQ(rom.at(0x1FFFF), 0x00);
    EXPECT_EQ(rom.at(0x20000), 0xFF);
}

TEST(ApplyFlashMethodPadding, LeavesEmptyFlashMethodAlone)
{
    std::vector<std::uint8_t> rom(0x100, 0xAA);
    rom = apply_flash_method_padding(std::move(rom), "");
    EXPECT_EQ(rom.size(), 0x100U);
}

// A run over `count` uint8 big-endian cells at address 0 with the identity
// expression, which is the shape most decode tests want.
ElementRun simple_run(std::uint32_t count, std::string_view from_byte = "x")
{
    return ElementRun{
        .address = 0,
        .count = count,
        .start_position = 1,
        .interval = 1,
        .storage_type = definition::StorageType::Uint8,
        .endian = "big",
        .from_byte = from_byte,
        .is_selectable = false,
    };
}

TEST(DecodeScaledValues, DecodesConsecutiveUint8Cells)
{
    const std::vector<std::uint8_t> rom{1, 2, 3};
    const auto result = decode_scaled_values(rom, simple_run(3), 15);
    ASSERT_TRUE(result.has_value());
    // Trailing comma after every value, including the last: legacy's format.
    EXPECT_EQ(*result, "1,2,3,");
}

TEST(DecodeScaledValues, AppliesFromByteExpression)
{
    const std::vector<std::uint8_t> rom{4};
    const auto result = decode_scaled_values(rom, simple_run(1, "x*0.5"), 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "2,");
}

TEST(DecodeScaledValues, HonoursStartPositionAndInterval)
{
    // start_position 2, interval 3, width 1: addresses 1, 4, 7.
    const std::vector<std::uint8_t> rom{0, 10, 0, 0, 20, 0, 0, 30};
    ElementRun run = simple_run(3);
    run.start_position = 2;
    run.interval = 3;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "10,20,30,");
}

TEST(DecodeScaledValues, DecodesBigEndianUint16)
{
    const std::vector<std::uint8_t> rom{0x12, 0x34};
    ElementRun run = simple_run(1);
    run.storage_type = definition::StorageType::Uint16;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "4660,"); // 0x1234
}

TEST(DecodeScaledValues, DecodesLittleEndianUint16)
{
    // DISCLOSED BEHAVIOR FIX vs. legacy. The legacy loop filled a union member
    // that the non-float value-selection path never read, so every
    // endian=="little" non-float run evaluated from_byte at x=0 -- a constant,
    // ignoring ROM bytes entirely. This decodes genuinely little-endian.
    const std::vector<std::uint8_t> rom{0x34, 0x12};
    ElementRun run = simple_run(1);
    run.storage_type = definition::StorageType::Uint16;
    run.endian = "little";

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "4660,"); // 0x1234
}

TEST(DecodeScaledValues, TreatsAnyNonLittleEndianStringAsBigEndian)
{
    // Legacy's split was `== "little"` / else, so "", "big", and anything else
    // all take the big-endian path. Reproduced exactly.
    const std::vector<std::uint8_t> rom{0x12, 0x34};
    for (std::string_view endian : {"big", "", "BIG", "nonsense"})
    {
        ElementRun run = simple_run(1);
        run.storage_type = definition::StorageType::Uint16;
        run.endian = endian;
        const auto result = decode_scaled_values(rom, run, 15);
        ASSERT_TRUE(result.has_value()) << endian;
        EXPECT_EQ(*result, "4660,") << endian;
    }
}

TEST(DecodeScaledValues, SignExtendsSignedStorageTypes)
{
    // DISCLOSED BEHAVIOR FIX vs. legacy, on every platform. Legacy's
    // `signedDataByte = (signedDataByte << 8) + FullRomData.at(...)` both
    // sign-extended intermediate bytes on a signed-char host (which includes
    // this project's arm64 macOS build) and never sign-extended the final
    // assembled value to its storage width. int16 0xFFFE rendered as -258 on
    // arm64 macOS and 65534 under -funsigned-char. Correct answer: -2.
    const std::vector<std::uint8_t> rom{0xFF, 0xFE};
    ElementRun run = simple_run(1);
    run.storage_type = definition::StorageType::Int16;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "-2,");
}

TEST(DecodeScaledValues, DoesNotSignExtendUnsignedStorageTypes)
{
    const std::vector<std::uint8_t> rom{0xFF, 0xFE};
    ElementRun run = simple_run(1);
    run.storage_type = definition::StorageType::Uint16;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "65534,");
}

TEST(DecodeScaledValues, DecodesInt8SignBit)
{
    const std::vector<std::uint8_t> rom{0x80};
    ElementRun run = simple_run(1);
    run.storage_type = definition::StorageType::Int8;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "-128,");
}

TEST(DecodeScaledValues, DecodesBigEndianFloat)
{
    // 1.5f == 0x3FC00000
    const std::vector<std::uint8_t> rom{0x3F, 0xC0, 0x00, 0x00};
    ElementRun run = simple_run(1);
    run.storage_type = definition::StorageType::Float;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "1.5,");
}

TEST(DecodeScaledValues, PreservesBigEndianFloatAssemblyWhenEndianSaysLittle)
{
    // Legacy selected its byte-reversal branch for every float, independent of
    // the endian field, and therefore treated float storage as big-endian on
    // the supported little-endian hosts.
    const std::vector<std::uint8_t> rom{0x3F, 0xC0, 0x00, 0x00};
    ElementRun run = simple_run(1);
    run.storage_type = definition::StorageType::Float;
    run.endian = "little";

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "1.5,");
}

TEST(DecodeScaledValues, EmitsZeroForSelectableRunsWithoutEvaluating)
{
    // is_selectable short-circuits before the expression runs, so even an
    // expression that would blow up yields the formatted text of 0.0.
    const std::vector<std::uint8_t> rom{7, 8};
    ElementRun run = simple_run(2, "x*999999");
    run.is_selectable = true;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "0,0,");
}

TEST(DecodeScaledValues, ReturnsEmptyStringForZeroCount)
{
    const std::vector<std::uint8_t> rom{1, 2, 3};
    const auto result = decode_scaled_values(rom, simple_run(0), 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "");
}

TEST(DecodeScaledValues, FailsWhenAnElementRunsPastTheRom)
{
    const std::vector<std::uint8_t> rom{1, 2};
    const auto result = decode_scaled_values(rom, simple_run(3), 15);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST(DecodeScaledValues, FailsWhenAMultiByteElementStraddlesTheEnd)
{
    const std::vector<std::uint8_t> rom{0x12};
    ElementRun run = simple_run(1);
    run.storage_type = definition::StorageType::Uint16;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST(DecodeScaledValues, RejectsMaximumAddressWithoutWrapping)
{
    const std::vector<std::uint8_t> rom{0x2A};
    ElementRun run = simple_run(1);
    run.address = std::numeric_limits<std::uint64_t>::max();

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST(DecodeScaledValues, ClampsZeroStartPositionDefensively)
{
    // definition_resolver rejects start_position == 0 (Task 4), so this cannot
    // arrive from a resolved definition. Clamped identically to
    // element_run_end's own guard so the two can never disagree if one is
    // reached directly.
    const std::vector<std::uint8_t> rom{1, 2};
    ElementRun run = simple_run(1);
    run.start_position = 0;

    const auto result = decode_scaled_values(rom, run, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "1,");
}

TEST(DecodeScaledValues, FormattingMatchesCapturedQtGroundTruth)
{
    // Ground truth captured from real QString::number(value, 'g', 15). This is
    // the highest-value test in this file: legacy formatted every decoded cell
    // with Qt's 'g', and MapData consumers compare text. Do not weaken it.
    //
    // from_byte is a bare numeric literal via std::format("{:.20f}", value),
    // not an expression referencing x, so this isolates formatting from byte
    // decoding. Fixed-point with 20 places specifically: std::to_string
    // truncates at 6 places, and a "{:.17g}" form emits scientific notation
    // whose 'e' the expression tokenizer silently misreads as a subtraction.
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
    const std::vector<std::uint8_t> rom{0x01};
    for (const Case& c : cases)
    {
        const std::string literal = std::format("{:.20f}", c.value);
        const auto result = decode_scaled_values(rom, simple_run(1, literal), 15);
        ASSERT_TRUE(result.has_value()) << c.expected;
        EXPECT_EQ(*result, std::string(c.expected) + ",") << "value=" << c.value;
    }
}

TEST(DecodeBloblistHex, HexEncodesRequestedBytes)
{
    const std::vector<std::uint8_t> rom{0x00, 0xAB, 0xCD, 0xEF};
    const auto result = decode_bloblist_hex(rom, 1, 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "abcdef");
}

TEST(DecodeBloblistHex, ReturnsEmptyStringForZeroCount)
{
    const std::vector<std::uint8_t> rom{0xAB};
    const auto result = decode_bloblist_hex(rom, 0, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "");
}

TEST(DecodeBloblistHex, FailsWhenTheRunExceedsTheRom)
{
    const std::vector<std::uint8_t> rom{0xAB, 0xCD};
    const auto result = decode_bloblist_hex(rom, 1, 3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

TEST(DecodeBloblistHex, RejectsMaximumAddressWithoutWrapping)
{
    const std::vector<std::uint8_t> rom{0xAB};

    const auto result = decode_bloblist_hex(rom, std::numeric_limits<std::uint64_t>::max(), 1);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

namespace
{
// A RomDefinition with one 3x1 uint8 map named "Fuel" at `address`, scaled by
// `expression`.
definition::RomDefinition one_map_definition(std::uint64_t address, std::string_view expression = "x")
{
    definition::RomDefinition rom;
    rom.scalings.push_back(definition::Scaling{.name = "FuelScaling", .from_byte = std::string(expression)});
    definition::CalibrationMap map;
    map.name = "Fuel";
    map.type = "2D";
    map.address = address;
    map.x_size = 3;
    map.y_size = 1;
    map.storage_type = definition::StorageType::Uint8;
    map.endian = "big";
    map.scaling_name = "FuelScaling";
    rom.maps.push_back(map);
    return rom;
}
} // namespace

TEST(ComputeMapCellValues, DecodesOneMapsCells)
{
    const definition::RomDefinition rom = one_map_definition(0);
    const std::vector<std::uint8_t> data{5, 6, 7};

    const auto result = compute_map_cell_values(rom, data, 15);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_FALSE(result->at(0).error.has_value());
    EXPECT_EQ(result->at(0).map_data, "5,6,7,");
    // x_size 3 but no usable x_axis type, y_size 1: both axes stay " ".
    EXPECT_EQ(result->at(0).x_axis_data, " ");
    EXPECT_EQ(result->at(0).y_axis_data, " ");
}

TEST(ComputeMapCellValues, UsesBlankExpressionWhenMapScalingIsAbsent)
{
    definition::RomDefinition rom = one_map_definition(0);
    rom.scalings.clear();
    rom.maps.at(0).scaling_name.clear();
    const std::vector<std::uint8_t> data{5, 6, 7};

    const auto result = compute_map_cell_values(rom, data, 15);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    ASSERT_FALSE(result->at(0).error.has_value());
    EXPECT_EQ(result->at(0).map_data, "0,0,0,");
}

TEST(ComputeMapCellValues, DecodesAnXAxisOfTypeXAxis)
{
    definition::RomDefinition rom = one_map_definition(0);
    rom.scalings.push_back(definition::Scaling{.name = "AxisScaling", .from_byte = "x"});
    rom.maps.at(0).x_axis.type = "X Axis";
    rom.maps.at(0).x_axis.address = 3;
    rom.maps.at(0).x_axis.size = 3;
    rom.maps.at(0).x_axis.storage_type = definition::StorageType::Uint8;
    rom.maps.at(0).x_axis.endian = "big";
    rom.maps.at(0).x_axis.scaling_name = "AxisScaling";

    const std::vector<std::uint8_t> data{5, 6, 7, 100, 200, 250};
    const auto result = compute_map_cell_values(rom, data, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->at(0).x_axis_data, "100,200,250,");
}

TEST(ComputeMapCellValues, UsesResolvedAxisExpressionAfterScalingInheritance)
{
    definition::RomDefinition rom = one_map_definition(0);
    // This is the valid resolved state pinned by DefinitionResolverTest.
    // A child scaling that supplies only units materializes as identity while
    // the inherited parent expression remains on AxisDefinition itself.
    rom.scalings.push_back(definition::Scaling{.name = "ChildAxisScaling", .units = "r/min", .from_byte = "x"});
    rom.maps.at(0).x_axis.type = "X Axis";
    rom.maps.at(0).x_axis.address = 3;
    rom.maps.at(0).x_axis.size = 3;
    rom.maps.at(0).x_axis.storage_type = definition::StorageType::Uint8;
    rom.maps.at(0).x_axis.endian = "big";
    rom.maps.at(0).x_axis.from_byte = "x*2";
    rom.maps.at(0).x_axis.scaling_name = "ChildAxisScaling";

    const std::vector<std::uint8_t> data{5, 6, 7, 10, 20, 30};
    const auto result = compute_map_cell_values(rom, data, 15);

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->at(0).error.has_value());
    EXPECT_EQ(result->at(0).x_axis_data, "20,40,60,");
}

TEST(ComputeMapCellValues, UsesStaticDataForStaticAxes)
{
    definition::RomDefinition rom = one_map_definition(0);
    rom.maps.at(0).x_axis.type = "Static X Axis";
    rom.maps.at(0).x_axis.static_data = {"1000", "2000", "3000"};

    const std::vector<std::uint8_t> data{5, 6, 7};
    const auto result = compute_map_cell_values(rom, data, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->at(0).x_axis_data, "1000,2000,3000,");
}

TEST(ComputeMapCellValues, LeavesUnrecognizedXAxisTypesUncomputed)
{
    definition::RomDefinition rom = one_map_definition(0);
    rom.maps.at(0).x_axis.type = "Something Else";
    rom.maps.at(0).x_axis.address = 3;
    rom.maps.at(0).x_axis.size = 3;

    const std::vector<std::uint8_t> data{5, 6, 7, 1, 2, 3};
    const auto result = compute_map_cell_values(rom, data, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->at(0).x_axis_data, " ");
}

TEST(ComputeMapCellValues, DecodesYAxisWithoutTypeBranching)
{
    // Legacy branches on x_axis.type but NOT on y_axis.type -- when y_size > 1
    // it always decodes. That asymmetry is real legacy behavior, reproduced
    // exactly rather than "fixed" to match the x-axis handling.
    definition::RomDefinition rom = one_map_definition(0);
    rom.scalings.push_back(definition::Scaling{.name = "AxisScaling", .from_byte = "x"});
    rom.maps.at(0).x_size = 1;
    rom.maps.at(0).y_size = 2;
    rom.maps.at(0).y_axis.type = "Something Unrecognized";
    rom.maps.at(0).y_axis.address = 2;
    rom.maps.at(0).y_axis.size = 2;
    rom.maps.at(0).y_axis.storage_type = definition::StorageType::Uint8;
    rom.maps.at(0).y_axis.endian = "big";
    rom.maps.at(0).y_axis.scaling_name = "AxisScaling";

    const std::vector<std::uint8_t> data{9, 9, 40, 50};
    const auto result = compute_map_cell_values(rom, data, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->at(0).y_axis_data, "40,50,");
}

TEST(ComputeMapCellValues, HexEncodesBloblistMaps)
{
    definition::RomDefinition rom;
    rom.scalings.push_back(definition::Scaling{.name = "Blob", .selections = {{"Off", "aabb"}, {"On", "ccdd"}}});
    definition::CalibrationMap map;
    map.name = "Switch";
    map.address = 1;
    map.x_size = 1;
    map.y_size = 1;
    map.storage_type = definition::StorageType::Bloblist;
    map.scaling_name = "Blob";
    rom.maps.push_back(map);

    // element_byte_size derives width 2 from the first selection ("aabb").
    const std::vector<std::uint8_t> data{0x00, 0xCC, 0xDD};
    const auto result = compute_map_cell_values(rom, data, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->at(0).map_data, "ccdd");
}

TEST(ComputeMapCellValues, DegradesPerMapWithoutFailingSiblings)
{
    definition::RomDefinition rom = one_map_definition(0);
    definition::CalibrationMap out_of_range = rom.maps.at(0);
    out_of_range.name = "Broken";
    out_of_range.address = 0xF0000000;
    rom.maps.push_back(out_of_range);

    const std::vector<std::uint8_t> data{5, 6, 7};
    const auto result = compute_map_cell_values(rom, data, 15);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2U);
    EXPECT_FALSE(result->at(0).error.has_value());
    EXPECT_EQ(result->at(0).map_data, "5,6,7,");
    ASSERT_TRUE(result->at(1).error.has_value());
    EXPECT_EQ(result->at(1).error->kind, ErrorKind::Internal);
    EXPECT_EQ(result->at(1).map_data, "");
}

TEST(ComputeMapCellValues, FlagsAMapWithNoAddress)
{
    definition::RomDefinition rom = one_map_definition(0);
    rom.maps.at(0).address = std::nullopt;

    const std::vector<std::uint8_t> data{5, 6, 7};
    const auto result = compute_map_cell_values(rom, data, 15);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->at(0).error.has_value());
    EXPECT_EQ(result->at(0).error->kind, ErrorKind::InvalidConfig);
}

TEST(ComputeMapCellValues, RejectsOverflowingMapCellCount)
{
    definition::RomDefinition rom = one_map_definition(0);
    rom.maps.at(0).x_size = 0x80000000U;
    rom.maps.at(0).y_size = 2;
    rom.maps.at(0).y_axis.address = 0;
    rom.maps.at(0).y_axis.storage_type = definition::StorageType::Uint8;
    const std::vector<std::uint8_t> data{1, 2};

    const auto result = compute_map_cell_values(rom, data, 15);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    ASSERT_TRUE(result->at(0).error.has_value());
    EXPECT_EQ(result->at(0).error->kind, ErrorKind::InvalidConfig);
}

TEST(ComputeMapCellValues, ReturnsEmptyListForADefinitionWithNoMaps)
{
    const auto result = compute_map_cell_values(definition::RomDefinition{}, {}, 15);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

} // namespace
} // namespace fastecu::calibration
