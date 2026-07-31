#include "src/backend/calibration/calibration_service.h"

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

} // namespace
} // namespace fastecu::calibration
