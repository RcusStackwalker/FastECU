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
std::vector<std::uint8_t> SyntheticRom()
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
        cfg_.PutSettings(config::testing::Setting("vehicle_id", "subaru-impreza-v1"));
        ASSERT_THAT(cfg_.Initialize(), IsOk());
    }

    void PutRom(const std::string& path, std::vector<std::uint8_t> rom)
    {
        cfg_.file_repository.files[path] = std::move(rom);
    }

    std::vector<std::string> Notices() const
    {
        return cfg_.events.notices;
    }

    std::vector<std::string> LogText() const
    {
        std::vector<std::string> text;
        for (const auto& [level, message] : cfg_.events.logs)
        {
            text.push_back(message);
        }
        return text;
    }

    config::testing::ConfigSessionFixture cfg_;
    InMemoryAtomicFileWriter writer_;
    definition::DefinitionService definitions_{cfg_.file_system, cfg_.file_repository, writer_};
    testing::FakeDefinitionCatalogs catalogs_;
    RomOpenUseCase opener_{catalogs_, definitions_, cfg_.file_repository, cfg_.file_system, cfg_.events, cfg_.session};
};

class RomOpenBasics : public RomOpenTest
{
};

TEST_F(RomOpenBasics, OpensAFileWithoutDefinitionsWhenBothFormatsAreDisabled)
{
    PutRom("/cal/dir/a.bin", SyntheticRom());

    const auto outcome = opener_.OpenFile("/cal/dir/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.source, (RomSource{"a.bin", "/cal/dir/a.bin", RomOrigin::kFile}));
    EXPECT_EQ(outcome->contents.rom, SyntheticRom());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(catalogs_.calls, IsEmpty());
    EXPECT_FALSE(outcome->size_rejected);
}

TEST_F(RomOpenBasics, APathWithoutABasenameIsShownAsDefaultBin)
{
    PutRom("/cal/dir/", SyntheticRom());

    const auto outcome = opener_.OpenFile("/cal/dir/");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.source.display_name, "default.bin");
}

TEST_F(RomOpenBasics, AnEmptyPathIsRejected)
{
    EXPECT_THAT(opener_.OpenFile(""), IsErr(ErrorKind::kInvalidConfig));
}

TEST_F(RomOpenBasics, AnUnreadableFileFailsWithTheLegacyNotice)
{
    cfg_.file_repository.read_errors["/cal/a.bin"] = Error{ErrorKind::kDisconnected, "gone"};

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsErr(ErrorKind::kDisconnected));
    EXPECT_THAT(Notices(), Contains("Calibration file: Unable to open calibration file for reading"));
    EXPECT_THAT(LogText(), Contains("Unable to open calibration file [Disconnected]: gone"));
}

TEST_F(RomOpenBasics, FileOpenDerivesProtocolInfoFromTheSelectedVehicle)
{
    // Row 0 (proto_a: checksum yes, mcu SH7058) is selected at startup, and
    // an empty flash method matches no vehicle.
    PutRom("/cal/a.bin", std::vector<std::uint8_t>(3 * 1024 + 5, 0));

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    const RomProtocolInfo& protocol = outcome->contents.protocol;
    EXPECT_FALSE(outcome->vehicle_selected);
    EXPECT_EQ(protocol.flash_method, "");
    EXPECT_EQ(protocol.checksum_module, "checksum"); // "checksum" + flash_method minus 3 chars
    EXPECT_EQ(protocol.mcu_type, "SH7058");
    EXPECT_EQ(protocol.file_size_label, "3kb");
    EXPECT_EQ(protocol.unpadded_size, 3U * 1024U + 5U);
    EXPECT_EQ(protocol.rom_id, "");
    EXPECT_EQ(*cfg_.session.SelectedRow(), 0U);
}

TEST_F(RomOpenBasics, AdoptingAReadImageBacksItUpAndSelectsItsProtocol)
{
    ReadImage image{
        .rom = SyntheticRom(),
        .filename = "A2WC522N2026-09-28_10h00m00s.bin",
        .rom_id = "A2WC522N",
        .protocol_name = "proto_b",
        .kernel_path = "/kernels/b.bin",
        .kernel_start_address = "0x0",
    };

    const auto outcome = opener_.AdoptReadImage(std::move(image));

    ASSERT_THAT(outcome, IsOk());
    const std::string backup = cfg_.session.EffectivePaths().calibration_files_directory + "read.bin";
    EXPECT_EQ(cfg_.file_repository.files.at(backup), SyntheticRom());
    EXPECT_EQ(outcome->contents.source.origin, RomOrigin::kEcuRead);
    EXPECT_EQ(outcome->contents.source.display_name, "A2WC522N2026-09-28_10h00m00s.bin");
    EXPECT_TRUE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg_.session.SelectedRow(), 1U);
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
    const std::size_t writes_before = cfg_.file_repository.write_calls.size();

    EXPECT_THAT(opener_.AdoptReadImage(ReadImage{.rom = SyntheticRom()}), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(opener_.AdoptReadImage(ReadImage{.filename = "x.bin"}), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(cfg_.file_repository.write_calls.size(), writes_before); // no backup written
}

TEST_F(RomOpenBasics, AFailedBackupDoesNotFailTheAdoption)
{
    const std::string backup = cfg_.session.EffectivePaths().calibration_files_directory + "read.bin";
    cfg_.file_repository.write_errors[backup] = Error{ErrorKind::kDisconnected, "disk full"};

    EXPECT_THAT(opener_.AdoptReadImage(ReadImage{.rom = SyntheticRom(), .filename = "x.bin"}), IsOk());
}

TEST_F(RomOpenBasics, PaddingFollowsTheUnpaddedSizeLabel)
{
    const auto outcome = opener_.AdoptReadImage(ReadImage{
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
    cfg_.session.Settings().use_ecuflash_definitions = "enabled";
    PutRom("/cal/a.bin", SyntheticRom());

    ASSERT_THAT(opener_.OpenFile("/cal/a.bin"), IsOk());
    EXPECT_THAT(catalogs_.calls, IsEmpty());
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

definition::DefinitionIndexEntry TestEntry(DefinitionFormat format, std::string source = "/defs/test.xml")
{
    return definition::DefinitionIndexEntry{
        .format = format,
        .definition_id = "TESTROM",
        .internal_id = "TESTROM",
        .internal_id_address = 0x10,
        .internal_id_encoding = definition::IdEncoding::kAscii,
        .source = std::move(source),
    };
}

class RomOpenDefinitions : public RomOpenTest
{
  protected:
    void SetUp() override
    {
        RomOpenTest::SetUp();
        cfg_.Put("/defs/test.xml", kTestDefinition);
        cfg_.file_system.files["/defs/test.xml"] = {};
        PutRom("/cal/a.bin", SyntheticRom());
    }

    void EnableEcuflashPrimary()
    {
        auto& settings = cfg_.session.Settings();
        settings.primary_definition_base = "ecuflash";
        settings.use_ecuflash_definitions = "enabled";
        settings.ecuflash_definition_files_directory = "/defs/";
    }

    void EnableRomraiderPrimary()
    {
        auto& settings = cfg_.session.Settings();
        settings.primary_definition_base = "romraider";
        settings.use_romraider_definitions = "enabled";
        settings.romraider_definition_files = {"/defs/test.xml"};
    }
};

TEST_F(RomOpenDefinitions, MatchesLoadsAndDecodesFromThePrimaryFormat)
{
    EnableEcuflashPrimary();
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {TestEntry(DefinitionFormat::kEcuFlash)};

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->format, DefinitionFormat::kEcuFlash);
    EXPECT_EQ(outcome->contents.definition->id, "TESTROM");
    EXPECT_EQ(outcome->contents.protocol.rom_id, "TESTROM");
    EXPECT_THAT(LogText(), Contains("EcuFlash cal id TESTROM found"));

    const CalibrationSession session(SessionId{1}, outcome->contents);
    const auto idle = session.DecodeMap(0);
    ASSERT_THAT(idle, IsOk());
    EXPECT_THAT(std::get<NumericRun>(idle->body).cells, ::testing::ElementsAre(fastecu::testing::IsOkAnd(42)));
}

TEST_F(RomOpenDefinitions, ADefinitionsFlashMethodAliasSelectsItsVehicle)
{
    EnableEcuflashPrimary();
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {TestEntry(DefinitionFormat::kEcuFlash)};

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.flash_method, "proto_a"); // alias_a -> proto_a
    EXPECT_TRUE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg_.session.SelectedRow(), 2U); // last row using proto_a
    EXPECT_THAT(LogText(), Contains("Alias: alias_a"));
    EXPECT_THAT(LogText(), Contains("Protocol: proto_a"));
    // proto_a's checksum is "yes": legacy's label drops three characters.
    EXPECT_EQ(outcome->contents.protocol.checksum_module, "checksumto_a");
}

TEST_F(RomOpenDefinitions, AnUnaliasedFlashMethodIsKept)
{
    EnableEcuflashPrimary();
    std::string text{kTestDefinition};
    text.replace(text.find("alias_a"), 7, "unknown_method");
    cfg_.Put("/defs/test.xml", text);
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {TestEntry(DefinitionFormat::kEcuFlash)};

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.flash_method, "unknown_method");
    EXPECT_FALSE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg_.session.SelectedRow(), 0U);
}

TEST_F(RomOpenDefinitions, TheSecondaryFormatIsTriedWhenThePrimaryFindsNothing)
{
    EnableEcuflashPrimary();
    cfg_.session.Settings().use_romraider_definitions = "enabled";
    cfg_.session.Settings().primary_definition_base = "ecuflash";
    catalogs_.entries[DefinitionFormat::kRomRaider] = {TestEntry(DefinitionFormat::kRomRaider)};
    cfg_.Put("/defs/test.xml", R"xml(<roms><rom><romid><xmlid>TESTROM</xmlid>
        <internalidaddress>10</internalidaddress><internalidstring>TESTROM</internalidstring></romid></rom></roms>)xml");

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(catalogs_.calls, (std::vector{DefinitionFormat::kEcuFlash, DefinitionFormat::kRomRaider}));
    EXPECT_THAT(LogText(), Contains(HasSubstr("Unable to match EcuFlash definition")));
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->format, DefinitionFormat::kRomRaider);
}

TEST_F(RomOpenDefinitions, RomRaiderPrimaryIsTriedFirst)
{
    EnableRomraiderPrimary();
    cfg_.session.Settings().use_ecuflash_definitions = "enabled";

    ASSERT_THAT(opener_.OpenFile("/cal/a.bin"), IsOk());

    EXPECT_EQ(catalogs_.calls, (std::vector{DefinitionFormat::kRomRaider, DefinitionFormat::kEcuFlash}));
}

TEST_F(RomOpenDefinitions, APrimaryHitSkipsTheSecondary)
{
    EnableEcuflashPrimary();
    cfg_.session.Settings().use_romraider_definitions = "enabled";
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {TestEntry(DefinitionFormat::kEcuFlash)};

    ASSERT_THAT(opener_.OpenFile("/cal/a.bin"), IsOk());

    EXPECT_EQ(catalogs_.calls, (std::vector{DefinitionFormat::kEcuFlash}));
}

TEST_F(RomOpenDefinitions, ACatalogFailureIsLoggedAndTheRomStillOpens)
{
    EnableEcuflashPrimary();
    catalogs_.errors[DefinitionFormat::kEcuFlash] = Error{ErrorKind::kDisconnected, "no dir"};

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(LogText(), Contains("Unable to match EcuFlash definition [Disconnected]: no dir"));
}

TEST_F(RomOpenDefinitions, EcuReportedIdIsUsedWhenMatchingFails)
{
    EnableEcuflashPrimary();
    definition::DefinitionIndexEntry entry = TestEntry(DefinitionFormat::kEcuFlash);
    entry.internal_id = "NOT-IN-ROM";
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {entry};

    const auto outcome = opener_.AdoptReadImage(ReadImage{
        .rom = SyntheticRom(),
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
    EnableEcuflashPrimary();
    definition::DefinitionIndexEntry entry = TestEntry(DefinitionFormat::kEcuFlash);
    entry.internal_id = "NOT-IN-ROM";
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {entry};

    const auto outcome = opener_.AdoptReadImage(ReadImage{
        .rom = SyntheticRom(),
        .filename = "x.bin",
        .rom_id = "OTHER",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.protocol.rom_id, "OTHER");
    EXPECT_THAT(Notices(), IsEmpty());
}

TEST_F(RomOpenDefinitions, MissingDefinitionFileOpensWithoutDefinitionAndNotifies)
{
    EnableEcuflashPrimary();
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {TestEntry(DefinitionFormat::kEcuFlash, "/defs/gone.xml")};

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(LogText(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(Notices(),
                Contains("Ecu definitions file: Unable to open ECU definition file /defs/gone.xml for reading"));
}

TEST_F(RomOpenDefinitions, AnUnparseableDefinitionThatExistsIsLoggedWithoutANotice)
{
    EnableEcuflashPrimary();
    cfg_.Put("/defs/test.xml", "<rom><romid>");
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {TestEntry(DefinitionFormat::kEcuFlash)};

    const auto outcome = opener_.OpenFile("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(LogText(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(Notices(), IsEmpty());
}

TEST_F(RomOpenDefinitions, SizeRejectionKeepsHeaderAndDropsMaps)
{
    EnableEcuflashPrimary();
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {TestEntry(DefinitionFormat::kEcuFlash)};
    std::vector<std::uint8_t> short_rom = SyntheticRom();
    short_rom.resize(0x20); // the map at 0x20 is now past the end
    PutRom("/cal/short.bin", short_rom);

    const auto outcome = opener_.OpenFile("/cal/short.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_TRUE(outcome->size_rejected);
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->definition.identity.xml_id, "TESTROM");
    EXPECT_THAT(outcome->contents.definition->definition.maps, IsEmpty());
    EXPECT_THAT(Notices(), Contains("File size error: Error in expected ROM size!"));
    EXPECT_THAT(LogText(), Contains(HasSubstr("Error in expected ROM size")));
}

// The production catalog is rebuilt on every open and skips unreadable files,
// so a definition deleted after startup is absent from it. Legacy still
// found it in the startup indexes and reported the missing file.
TEST_F(RomOpenDefinitions, AnIndexedDefinitionMissingFromTheCatalogNotifiesOnEcuRead)
{
    EnableEcuflashPrimary();
    catalogs_.indexed_sources[{DefinitionFormat::kEcuFlash, "TESTROM"}] = "/defs/gone.xml";

    const auto outcome = opener_.AdoptReadImage(ReadImage{
        .rom = SyntheticRom(),
        .filename = "x.bin",
        .rom_id = "TESTROM",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(LogText(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(Notices(),
                Contains("Ecu definitions file: Unable to open ECU definition file /defs/gone.xml for reading"));
}

TEST_F(RomOpenDefinitions, AnIndexedDefinitionThatStillExistsIsLoggedWithoutANotice)
{
    EnableEcuflashPrimary();
    catalogs_.indexed_sources[{DefinitionFormat::kEcuFlash, "TESTROM"}] = "/defs/test.xml";

    const auto outcome = opener_.AdoptReadImage(ReadImage{
        .rom = SyntheticRom(),
        .filename = "x.bin",
        .rom_id = "TESTROM",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(LogText(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(Notices(), IsEmpty());
}

} // namespace
} // namespace fastecu::calibration
