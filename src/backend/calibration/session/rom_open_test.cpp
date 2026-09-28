#include "src/backend/calibration/session/rom_open.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/testing/fake_definition_catalogs.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{

using definition::DefinitionFormat;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

// A synthetic 64-byte image: "TESTROM" at 0x10, 0x2A at 0x20.
std::vector<std::uint8_t> synthetic_rom()
{
    std::vector<std::uint8_t> rom(64, 0);
    const std::string id = "TESTROM";
    std::ranges::copy(id, rom.begin() + 0x10);
    rom[0x20] = 0x2A;
    return rom;
}

class RomOpenTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg.initialize(), IsOk());
    }

    void put_rom(const std::string& path, std::vector<std::uint8_t> rom)
    {
        cfg.file_repository.files[path] = std::move(rom);
    }

    std::vector<std::string> notices() const
    {
        return cfg.events.notices;
    }

    std::vector<std::string> log_text() const
    {
        std::vector<std::string> text;
        for (const auto& [level, message] : cfg.events.logs)
        {
            text.push_back(message);
        }
        return text;
    }

    config::testing::ConfigSessionFixture cfg;
    InMemoryAtomicFileWriter writer;
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    testing::FakeDefinitionCatalogs catalogs;
    RomOpenUseCase opener{catalogs, definitions, cfg.file_repository, cfg.file_system, cfg.events, cfg.session};
};

class RomOpenBasics : public RomOpenTest
{
};

TEST_F(RomOpenBasics, OpensAFileWithoutDefinitionsWhenBothFormatsAreDisabled)
{
    put_rom("/cal/dir/a.bin", synthetic_rom());

    const auto outcome = opener.open_file("/cal/dir/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.source, (RomSource{"a.bin", "/cal/dir/a.bin", RomOrigin::File}));
    EXPECT_EQ(outcome->contents.rom, synthetic_rom());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(catalogs.calls, IsEmpty());
    EXPECT_FALSE(outcome->size_rejected);
}

TEST_F(RomOpenBasics, APathWithoutABasenameIsShownAsDefaultBin)
{
    put_rom("/cal/dir/", synthetic_rom());

    const auto outcome = opener.open_file("/cal/dir/");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.source.display_name, "default.bin");
}

TEST_F(RomOpenBasics, AnEmptyPathIsRejected)
{
    EXPECT_THAT(opener.open_file(""), IsErr(ErrorKind::InvalidConfig));
}

TEST_F(RomOpenBasics, AnUnreadableFileFailsWithTheLegacyNotice)
{
    cfg.file_repository.read_errors["/cal/a.bin"] = Error{ErrorKind::Disconnected, "gone"};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsErr(ErrorKind::Disconnected));
    EXPECT_THAT(notices(), Contains("Calibration file: Unable to open calibration file for reading"));
    EXPECT_THAT(log_text(), Contains("Unable to open calibration file [Disconnected]: gone"));
}

TEST_F(RomOpenBasics, FileOpenDerivesProtocolInfoFromTheSelectedVehicle)
{
    // Row 0 (proto_a: checksum yes, mcu SH7058) is selected at startup, and
    // an empty flash method matches no vehicle.
    put_rom("/cal/a.bin", std::vector<std::uint8_t>(3 * 1024 + 5, 0));

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    const RomProtocolInfo& protocol = outcome->contents.protocol;
    EXPECT_FALSE(outcome->vehicle_selected);
    EXPECT_EQ(protocol.flash_method, "");
    EXPECT_EQ(protocol.checksum_module, "checksum"); // "checksum" + flash_method minus 3 chars
    EXPECT_EQ(protocol.mcu_type, "SH7058");
    EXPECT_EQ(protocol.file_size_label, "3kb");
    EXPECT_EQ(protocol.unpadded_size, 3U * 1024U + 5U);
    EXPECT_EQ(protocol.rom_id, "");
    EXPECT_EQ(*cfg.session.selected_row(), 0U);
}

TEST_F(RomOpenBasics, AdoptingAReadImageBacksItUpAndSelectsItsProtocol)
{
    ReadImage image{
        .rom = synthetic_rom(),
        .filename = "A2WC522N2026-09-28_10h00m00s.bin",
        .rom_id = "A2WC522N",
        .protocol_name = "proto_b",
        .kernel_path = "/kernels/b.bin",
        .kernel_start_address = "0x0",
    };

    const auto outcome = opener.adopt_read_image(std::move(image));

    ASSERT_THAT(outcome, IsOk());
    const std::string backup = cfg.session.effective_paths().calibration_files_directory + "read.bin";
    EXPECT_EQ(cfg.file_repository.files.at(backup), synthetic_rom());
    EXPECT_EQ(outcome->contents.source.origin, RomOrigin::EcuRead);
    EXPECT_EQ(outcome->contents.source.display_name, "A2WC522N2026-09-28_10h00m00s.bin");
    EXPECT_TRUE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg.session.selected_row(), 1U);
    const RomProtocolInfo& protocol = outcome->contents.protocol;
    EXPECT_EQ(protocol.flash_method, "proto_b");
    EXPECT_EQ(protocol.rom_id, "A2WC522N");
    EXPECT_EQ(protocol.checksum_module, "Not implemented yet"); // proto_b checksum n/a
    EXPECT_EQ(protocol.mcu_type, "M32R");
    EXPECT_EQ(protocol.kernel_path, "/kernels/b.bin");
    EXPECT_EQ(protocol.kernel_start_address, "0x0");
}

TEST_F(RomOpenBasics, AdoptionNeedsAFilenameAndBytes)
{
    const std::size_t writes_before = cfg.file_repository.write_calls.size();

    EXPECT_THAT(opener.adopt_read_image(ReadImage{.rom = synthetic_rom()}), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(opener.adopt_read_image(ReadImage{.filename = "x.bin"}), IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(cfg.file_repository.write_calls.size(), writes_before); // no backup written
}

TEST_F(RomOpenBasics, AFailedBackupDoesNotFailTheAdoption)
{
    const std::string backup = cfg.session.effective_paths().calibration_files_directory + "read.bin";
    cfg.file_repository.write_errors[backup] = Error{ErrorKind::Disconnected, "disk full"};

    EXPECT_THAT(opener.adopt_read_image(ReadImage{.rom = synthetic_rom(), .filename = "x.bin"}), IsOk());
}

TEST_F(RomOpenBasics, PaddingFollowsTheUnpaddedSizeLabel)
{
    const auto outcome = opener.adopt_read_image(ReadImage{
        .rom = std::vector<std::uint8_t>(0x100, 0x11),
        .filename = "x.bin",
        .protocol_name = "sub_ecu_denso_mc68hc16y5_02",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.file_size_label, "0kb");
    EXPECT_EQ(outcome->contents.protocol.unpadded_size, 0x100U);
    EXPECT_EQ(outcome->contents.rom.size(), 0x28000U); // zero-extended to 0x20000, then 0x8000 of 0xFF
}

TEST_F(RomOpenBasics, NoCatalogIsRequestedWhileTheEcuFlashDirectoryIsEmpty)
{
    // EcuFlash is primary by default but needs a directory to be consulted.
    cfg.session.settings().use_ecuflash_definitions = "enabled";
    put_rom("/cal/a.bin", synthetic_rom());

    ASSERT_THAT(opener.open_file("/cal/a.bin"), IsOk());
    EXPECT_THAT(catalogs.calls, IsEmpty());
}

} // namespace
} // namespace fastecu::calibration
