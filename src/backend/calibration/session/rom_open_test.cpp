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
        // The startup vehicle gate selects a vehicle before any ROM opens;
        // row 0 (subaru-impreza-v1) stands in for that choice.
        cfg.put_settings(config::testing::setting("vehicle_id", "subaru-impreza-v1"));
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

// A one-map EcuFlash definition identified by "TESTROM" at 0x10, whose
// flash method is proto_a's alias. Its map is the single byte at 0x20.
constexpr std::string_view kTestDefinition = R"xml(
<rom>
  <romid><xmlid>TESTROM</xmlid><internalidaddress>10</internalidaddress>
    <internalidstring>TESTROM</internalidstring><make>Subaru</make>
    <flashmethod>alias_a</flashmethod><checksummodule>def-module</checksummodule></romid>
  <scaling name="Raw" toexpr="x" frexpr="x" format="%d" storagetype="uint8" endian="big"/>
  <table name="Idle" address="20" type="1D" scaling="Raw" storagetype="uint8"/>
</rom>)xml";

definition::DefinitionIndexEntry test_entry(DefinitionFormat format, std::string source = "/defs/test.xml")
{
    return definition::DefinitionIndexEntry{
        .format = format,
        .definition_id = "TESTROM",
        .internal_id = "TESTROM",
        .internal_id_address = 0x10,
        .internal_id_encoding = definition::IdEncoding::Ascii,
        .source = std::move(source),
    };
}

class RomOpenDefinitions : public RomOpenTest
{
  protected:
    void SetUp() override
    {
        RomOpenTest::SetUp();
        cfg.put("/defs/test.xml", kTestDefinition);
        cfg.file_system.files["/defs/test.xml"] = {};
        put_rom("/cal/a.bin", synthetic_rom());
    }

    void enable_ecuflash_primary()
    {
        auto& settings = cfg.session.settings();
        settings.primary_definition_base = "ecuflash";
        settings.use_ecuflash_definitions = "enabled";
        settings.ecuflash_definition_files_directory = "/defs/";
    }

    void enable_romraider_primary()
    {
        auto& settings = cfg.session.settings();
        settings.primary_definition_base = "romraider";
        settings.use_romraider_definitions = "enabled";
        settings.romraider_definition_files = {"/defs/test.xml"};
    }
};

TEST_F(RomOpenDefinitions, MatchesLoadsAndDecodesFromThePrimaryFormat)
{
    enable_ecuflash_primary();
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->format, DefinitionFormat::EcuFlash);
    EXPECT_EQ(outcome->contents.definition->id, "TESTROM");
    EXPECT_EQ(outcome->contents.protocol.rom_id, "TESTROM");
    EXPECT_THAT(log_text(), Contains("EcuFlash cal id TESTROM found"));

    const CalibrationSession session(SessionId{1}, outcome->contents);
    const auto idle = session.decode_map(0);
    ASSERT_THAT(idle, IsOk());
    EXPECT_THAT(std::get<NumericRun>(idle->body).cells, ::testing::ElementsAre(fastecu::testing::IsOkAnd(42)));
}

TEST_F(RomOpenDefinitions, ADefinitionsFlashMethodAliasSelectsItsVehicle)
{
    enable_ecuflash_primary();
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.flash_method, "proto_a"); // alias_a -> proto_a
    EXPECT_TRUE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg.session.selected_row(), 2U); // last row using proto_a
    EXPECT_THAT(log_text(), Contains("Alias: alias_a"));
    EXPECT_THAT(log_text(), Contains("Protocol: proto_a"));
    // proto_a's checksum is "yes": legacy's label drops three characters.
    EXPECT_EQ(outcome->contents.protocol.checksum_module, "checksumto_a");
}

TEST_F(RomOpenDefinitions, AnUnaliasedFlashMethodIsKept)
{
    enable_ecuflash_primary();
    std::string text{kTestDefinition};
    text.replace(text.find("alias_a"), 7, "unknown_method");
    cfg.put("/defs/test.xml", text);
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.flash_method, "unknown_method");
    EXPECT_FALSE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg.session.selected_row(), 0U);
}

TEST_F(RomOpenDefinitions, TheSecondaryFormatIsTriedWhenThePrimaryFindsNothing)
{
    enable_ecuflash_primary();
    cfg.session.settings().use_romraider_definitions = "enabled";
    cfg.session.settings().primary_definition_base = "ecuflash";
    catalogs.entries[DefinitionFormat::RomRaider] = {test_entry(DefinitionFormat::RomRaider)};
    cfg.put("/defs/test.xml", R"xml(<roms><rom><romid><xmlid>TESTROM</xmlid>
        <internalidaddress>10</internalidaddress><internalidstring>TESTROM</internalidstring></romid></rom></roms>)xml");

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(catalogs.calls, (std::vector{DefinitionFormat::EcuFlash, DefinitionFormat::RomRaider}));
    EXPECT_THAT(log_text(), Contains(HasSubstr("Unable to match EcuFlash definition")));
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->format, DefinitionFormat::RomRaider);
}

TEST_F(RomOpenDefinitions, RomRaiderPrimaryIsTriedFirst)
{
    enable_romraider_primary();
    cfg.session.settings().use_ecuflash_definitions = "enabled";

    ASSERT_THAT(opener.open_file("/cal/a.bin"), IsOk());

    EXPECT_EQ(catalogs.calls, (std::vector{DefinitionFormat::RomRaider, DefinitionFormat::EcuFlash}));
}

TEST_F(RomOpenDefinitions, APrimaryHitSkipsTheSecondary)
{
    enable_ecuflash_primary();
    cfg.session.settings().use_romraider_definitions = "enabled";
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    ASSERT_THAT(opener.open_file("/cal/a.bin"), IsOk());

    EXPECT_EQ(catalogs.calls, (std::vector{DefinitionFormat::EcuFlash}));
}

TEST_F(RomOpenDefinitions, ACatalogFailureIsLoggedAndTheRomStillOpens)
{
    enable_ecuflash_primary();
    catalogs.errors[DefinitionFormat::EcuFlash] = Error{ErrorKind::Disconnected, "no dir"};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(log_text(), Contains("Unable to match EcuFlash definition [Disconnected]: no dir"));
}

TEST_F(RomOpenDefinitions, EcuReportedIdIsUsedWhenMatchingFails)
{
    enable_ecuflash_primary();
    definition::DefinitionIndexEntry entry = test_entry(DefinitionFormat::EcuFlash);
    entry.internal_id = "NOT-IN-ROM";
    catalogs.entries[DefinitionFormat::EcuFlash] = {entry};

    const auto outcome = opener.adopt_read_image(ReadImage{
        .rom = synthetic_rom(),
        .filename = "x.bin",
        .rom_id = "TESTROM",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->id, "TESTROM");
    EXPECT_EQ(outcome->contents.protocol.rom_id, "TESTROM");
}

TEST_F(RomOpenDefinitions, AnIdOutsideTheCatalogEndsTheAttemptSilently)
{
    enable_ecuflash_primary();
    definition::DefinitionIndexEntry entry = test_entry(DefinitionFormat::EcuFlash);
    entry.internal_id = "NOT-IN-ROM";
    catalogs.entries[DefinitionFormat::EcuFlash] = {entry};

    const auto outcome = opener.adopt_read_image(ReadImage{
        .rom = synthetic_rom(),
        .filename = "x.bin",
        .rom_id = "OTHER",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.protocol.rom_id, "OTHER");
    EXPECT_THAT(notices(), IsEmpty());
}

TEST_F(RomOpenDefinitions, MissingDefinitionFileOpensWithoutDefinitionAndNotifies)
{
    enable_ecuflash_primary();
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash, "/defs/gone.xml")};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(log_text(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(notices(),
                Contains("Ecu definitions file: Unable to open ECU definition file /defs/gone.xml for reading"));
}

TEST_F(RomOpenDefinitions, AnUnparseableDefinitionThatExistsIsLoggedWithoutANotice)
{
    enable_ecuflash_primary();
    cfg.put("/defs/test.xml", "<rom><romid>");
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(log_text(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(notices(), IsEmpty());
}

TEST_F(RomOpenDefinitions, SizeRejectionKeepsHeaderAndDropsMaps)
{
    enable_ecuflash_primary();
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};
    std::vector<std::uint8_t> short_rom = synthetic_rom();
    short_rom.resize(0x20); // the map at 0x20 is now past the end
    put_rom("/cal/short.bin", short_rom);

    const auto outcome = opener.open_file("/cal/short.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_TRUE(outcome->size_rejected);
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->definition.identity.xml_id, "TESTROM");
    EXPECT_THAT(outcome->contents.definition->definition.maps, IsEmpty());
    EXPECT_THAT(notices(), Contains("File size error: Error in expected ROM size!"));
    EXPECT_THAT(log_text(), Contains(HasSubstr("Error in expected ROM size")));
}

// The production catalog is rebuilt on every open and skips unreadable files,
// so a definition deleted after startup is absent from it. Legacy still
// found it in the startup indexes and reported the missing file.
TEST_F(RomOpenDefinitions, AnIndexedDefinitionMissingFromTheCatalogNotifiesOnEcuRead)
{
    enable_ecuflash_primary();
    catalogs.indexed_sources[{DefinitionFormat::EcuFlash, "TESTROM"}] = "/defs/gone.xml";

    const auto outcome = opener.adopt_read_image(ReadImage{
        .rom = synthetic_rom(),
        .filename = "x.bin",
        .rom_id = "TESTROM",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(log_text(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(notices(),
                Contains("Ecu definitions file: Unable to open ECU definition file /defs/gone.xml for reading"));
}

TEST_F(RomOpenDefinitions, AnIndexedDefinitionThatStillExistsIsLoggedWithoutANotice)
{
    enable_ecuflash_primary();
    catalogs.indexed_sources[{DefinitionFormat::EcuFlash, "TESTROM"}] = "/defs/test.xml";

    const auto outcome = opener.adopt_read_image(ReadImage{
        .rom = synthetic_rom(),
        .filename = "x.bin",
        .rom_id = "TESTROM",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(log_text(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(notices(), IsEmpty());
}

} // namespace
} // namespace fastecu::calibration
