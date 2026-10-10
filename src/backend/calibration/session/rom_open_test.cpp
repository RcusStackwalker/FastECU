#include "src/backend/calibration/session/rom_open.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/backend/calibration/session/testing/fake_definition_catalogs.h"
#include "src/backend/config/catalog.h"
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
using ::testing::ElementsAreArray;
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
    EXPECT_THAT(outcome->contents.image.File(), ElementsAreArray(SyntheticRom()));
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(catalogs_.calls, IsEmpty());
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

// A protocol this catalog does not know places the ROM file at address 0 as
// loaded: nothing is inserted, whatever the flash method is called.
TEST_F(RomOpenBasics, AFileOfAnUnknownProtocolOpensAsLoaded)
{
    const auto outcome = opener_.AdoptReadImage(ReadImage{
        .rom = std::vector<std::uint8_t>(0x100, 0x11),
        .filename = "x.bin",
        .protocol_name = "sub_ecu_denso_mc68hc16y5_02",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.file_size_label, "0kb");
    EXPECT_EQ(outcome->contents.image.File().size(), 0x100U);
    EXPECT_EQ(outcome->contents.image.Map().FileSize(), memory::ByteCount{0x100});
}

TEST_F(RomOpenBasics, AnEmptyFileIsNotOpened)
{
    PutRom("/cal/empty.bin", {});

    EXPECT_THAT(opener_.OpenFile("/cal/empty.bin"), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Notices(), Contains("File size error: the ROM file is empty"));
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
        .internal_id_address = memory::DefinitionAddress{0x10},
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

// Q17: a map past the end of the file is a structural failure of that map
// alone; the definition keeps its maps and the file opens without a notice.
TEST_F(RomOpenDefinitions, AMapPastTheEndOfTheFileFailsOnlyThatMap)
{
    EnableEcuflashPrimary();
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {TestEntry(DefinitionFormat::kEcuFlash)};
    std::vector<std::uint8_t> short_rom = SyntheticRom();
    short_rom.resize(0x20); // the map at 0x20 is now past the end
    PutRom("/cal/short.bin", short_rom);

    auto outcome = opener_.OpenFile("/cal/short.bin");

    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->definition.maps.size(), 1U);
    EXPECT_THAT(Notices(), IsEmpty());
    const CalibrationSession session(SessionId{1}, std::move(outcome->contents));
    EXPECT_THAT(session.DecodeMap(0), IsErr(ErrorKind::kInvalidConfig));
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

// "mapped": a 0x40-byte file with a 0x10-byte fill block between its halves,
// like MC68HC16Y5 `_02`. "based": a 0x40-byte file at 0x1000 whose definitions
// count from 0x1000, like 1N83M.
constexpr auto kMappedBlocks = std::to_array({
    config::FileBlock(0x00, 0x20, 0x00, memory::Writability::kWritable),
    config::FillBlock(0x20, 0x10, 0xFF),
    config::FileBlock(0x30, 0x20, 0x20, memory::Writability::kWritable),
});
constexpr auto kBasedBlocks = std::to_array({config::FileBlock(0x1000, 0x40, 0x00, memory::Writability::kWritable)});
constexpr auto kMappedMaps =
    std::to_array<config::MemoryMapSpec>({{.file_size = memory::ByteCount{0x40}, .blocks = kMappedBlocks}});
constexpr auto kBasedMaps = std::to_array<config::MemoryMapSpec>(
    {{.file_size = memory::ByteCount{0x40}, .blocks = kBasedBlocks, .definition_base = memory::FlashAddress{0x1000}}});
constexpr auto kMappedProtocols = std::to_array<config::ProtocolSpec>({
    {.name = "proto_a", .mcu = "SH7058", .checksum = config::ChecksumSupport::kCorrected, .description = "A"},
    {.name = "mapped",
     .alias = "mapped_alias",
     .mcu = "MC68HC16Y5",
     .description = "Mapped",
     .memory_maps = kMappedMaps},
    {.name = "based", .mcu = "N83M_4MB", .description = "Based", .memory_maps = kBasedMaps},
});
constexpr auto kMappedVehicles = std::to_array<config::VehicleSpec>({
    {.id = "subaru-impreza-v1", .make = "Subaru", .protocol = config::ProtocolIn(kMappedProtocols, "proto_a")},
    {.id = "mapped-car", .make = "Subaru", .protocol = config::ProtocolIn(kMappedProtocols, "mapped")},
    {.id = "based-car", .make = "Subaru", .protocol = config::ProtocolIn(kMappedProtocols, "based")},
});
constexpr config::Catalog kMappedCatalog{kMappedProtocols, kMappedVehicles};

class RomOpenMemoryMaps : public RomOpenTest
{
  protected:
    void SetUp() override
    {
        cfg_.catalog = kMappedCatalog;
        RomOpenTest::SetUp();
    }
};

// Each byte holds its own file offset.
std::vector<std::uint8_t> CountingRom(std::size_t size)
{
    std::vector<std::uint8_t> rom(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        rom[index] = static_cast<std::uint8_t>(index);
    }
    return rom;
}

TEST_F(RomOpenMemoryMaps, AReadImageIsPlacedByItsProtocolsMemoryMap)
{
    auto outcome =
        opener_.AdoptReadImage(ReadImage{.rom = CountingRom(0x40), .filename = "x.bin", .protocol_name = "mapped"});

    ASSERT_THAT(outcome, IsOk());
    const CalibrationSession session(SessionId{1}, std::move(outcome->contents));
    EXPECT_THAT(session.File(), ElementsAreArray(CountingRom(0x40)));
    ASSERT_EQ(session.Rom().size(), 0x50U);
    EXPECT_EQ(session.Rom()[0x1F], 0x1F);
    EXPECT_EQ(session.Rom()[0x20], 0xFF);
    EXPECT_EQ(session.Rom()[0x30], 0x20);
}

TEST_F(RomOpenMemoryMaps, DefinitionAddressesCountFromTheMapsDefinitionBase)
{
    auto outcome =
        opener_.AdoptReadImage(ReadImage{.rom = CountingRom(0x40), .filename = "x.bin", .protocol_name = "based"});

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.image.Map().DefinitionBase(), memory::FlashAddress{0x1000});
    const CalibrationSession session(SessionId{1}, std::move(outcome->contents));
    EXPECT_THAT(session.Rom(), ElementsAreArray(CountingRom(0x40)));
}

TEST_F(RomOpenMemoryMaps, AFileSizeTheProtocolHasNoMapForIsNotOpened)
{
    const auto outcome =
        opener_.AdoptReadImage(ReadImage{.rom = CountingRom(0x41), .filename = "x.bin", .protocol_name = "mapped"});

    EXPECT_THAT(outcome, IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Notices(), Contains("File size error: protocol 'mapped' has no memory map for 0x41-byte ROM files"));
    EXPECT_EQ(*cfg_.session.SelectedRow(), 0U); // the rejected file selected no vehicle
}

// A "mapped" EcuFlash definition identified by "MAPPED" at ECU 0x30, which is
// file offset 0x20 under the mapped protocol's memory map.
constexpr std::string_view kMappedDefinition = R"xml(
<rom>
  <romid><xmlid>MAPPED</xmlid><internalidaddress>30</internalidaddress>
    <internalidstring>MAPPED</internalidstring><flashmethod>mapped</flashmethod></romid>
</rom>)xml";

class RomOpenMappedDefinitions : public RomOpenMemoryMaps
{
  protected:
    void SetUp() override
    {
        RomOpenMemoryMaps::SetUp();
        cfg_.Put("/defs/mapped.xml", kMappedDefinition);
        cfg_.file_system.files["/defs/mapped.xml"] = {};
        auto& settings = cfg_.session.Settings();
        settings.primary_definition_base = "ecuflash";
        settings.use_ecuflash_definitions = "enabled";
        settings.ecuflash_definition_files_directory = "/defs/";
        catalogs_.entries[DefinitionFormat::kEcuFlash] = {MappedEntry("mapped")};
    }

    static definition::DefinitionIndexEntry MappedEntry(std::string flash_method)
    {
        return definition::DefinitionIndexEntry{
            .format = DefinitionFormat::kEcuFlash,
            .definition_id = "MAPPED",
            .internal_id = "MAPPED",
            .internal_id_address = memory::DefinitionAddress{0x30},
            .internal_id_encoding = definition::IdEncoding::kAscii,
            .flash_method = std::move(flash_method),
            .source = "/defs/mapped.xml",
        };
    }

    static std::vector<std::uint8_t> RomWithIdAt(std::size_t size, std::size_t offset)
    {
        std::vector<std::uint8_t> rom(size, 0);
        const std::string id = "MAPPED";
        std::ranges::copy(id, rom.begin() + static_cast<std::ptrdiff_t>(offset));
        return rom;
    }
};

TEST_F(RomOpenMappedDefinitions, TheInternalIdIsReadThroughTheDefinitionsMemoryMap)
{
    PutRom("/cal/m.bin", RomWithIdAt(0x40, 0x20));

    const auto outcome = opener_.OpenFile("/cal/m.bin");

    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->id, "MAPPED");
    EXPECT_EQ(outcome->contents.protocol.flash_method, "mapped");
}

TEST_F(RomOpenMappedDefinitions, AnIdAtTheFileOffsetAloneDoesNotMatch)
{
    // File offset 0x30 is ECU 0x40 under the map, not 0x30.
    PutRom("/cal/m.bin", RomWithIdAt(0x40, 0x30));

    const auto outcome = opener_.OpenFile("/cal/m.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
}

TEST_F(RomOpenMappedDefinitions, AFlashMethodAliasSelectsItsProtocolsMemoryMap)
{
    catalogs_.entries[DefinitionFormat::kEcuFlash] = {MappedEntry("mapped_alias")};
    PutRom("/cal/m.bin", RomWithIdAt(0x40, 0x20));

    const auto outcome = opener_.OpenFile("/cal/m.bin");

    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->id, "MAPPED");
}

TEST_F(RomOpenMappedDefinitions, AFileSizeTheMatchedProtocolHasNoMapForIsStillIdentifiedThenRejected)
{
    // No map for 0x41 bytes: matching falls back to the file offset, so the
    // definition is found and the open names the real problem.
    PutRom("/cal/m.bin", RomWithIdAt(0x41, 0x30));

    const auto outcome = opener_.OpenFile("/cal/m.bin");

    EXPECT_THAT(outcome, IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(Notices(), Contains("File size error: protocol 'mapped' has no memory map for 0x41-byte ROM files"));
    EXPECT_EQ(*cfg_.session.SelectedRow(), 0U);
}

} // namespace
} // namespace fastecu::calibration
