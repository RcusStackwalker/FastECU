#include "src/backend/calibration/calibration_service.h"

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

    EXPECT_FALSE(
        validate_rom_size(definition_with_one_map(map, {scaling}), 0x1000).has_value());
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

} // namespace
} // namespace fastecu::calibration
