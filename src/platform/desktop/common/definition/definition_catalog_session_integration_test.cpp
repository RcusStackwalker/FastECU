// Catalog/file characterization migrated from src/backend/definitions/file_actions.cpp.
#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/definition/definition_catalog_session.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"

namespace
{
using fastecu::definition::DefinitionFormat;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using testing::ElementsAre;
using testing::Field;

fastecu::definition::DefinitionHeaderInput Header(std::string id = "NEW_XML")
{
    return {.xml_id = std::move(id),
            .internal_id = "A1B2C3",
            .ecu_id = "ECU-42",
            .internal_id_address = 0x1A0,
            .metadata = {.make = "Subaru",
                         .model = "Legacy",
                         .flash_method = "proto_a",
                         .file_size = "512",
                         .notes = "Header notes"},
            .notes = "Document notes"};
}

class DefinitionCatalogSessionIntegration : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(root_.isValid());
        ASSERT_THAT(config_.Initialize(), IsOk());
        ASSERT_TRUE(QDir().mkpath(root_.filePath("defs")));
        config_.session.Settings().ecuflash_definition_files_directory = Path("defs");
    }

    std::string Path(const char *name) const
    {
        return root_.filePath(name).toStdString();
    }

    QTemporaryDir root_;
    fastecu::config::testing::ConfigSessionFixture config_;
    QtFileSystem file_system_;
    QtFileRepository files_;
    QtAtomicFileWriter writer_;
    fastecu::definition::DefinitionService service_{file_system_, files_, writer_};
    fastecu::definition::DefinitionCatalogSession session_{service_, config_.session, file_system_, config_.events};
};

TEST_F(DefinitionCatalogSessionIntegration, CreatedFileOutsideDirectoryRoundTripsAndRemainsDiscoverable)
{
    auto input = Header();
    input.include = "BASE_XML";
    ASSERT_THAT(session_.SubmitNewDefinition(Path("outside.xml"), input, true), IsOk());
    auto bytes = files_.Read(Path("outside.xml"));
    ASSERT_THAT(bytes, IsOk());
    auto parsed = fastecu::definition::ParseEcuflashDefinition(*bytes, Path("outside.xml"));
    ASSERT_THAT(parsed, IsOk());
    EXPECT_EQ(parsed->identity, (fastecu::definition::RomIdentity{"NEW_XML", "A1B2C3", "ECU-42", 0x1A0}));
    EXPECT_EQ(parsed->metadata.make, "Subaru");
    EXPECT_EQ(parsed->metadata.model, "Legacy");
    EXPECT_EQ(parsed->metadata.flash_method, "proto_a");
    EXPECT_EQ(parsed->metadata.file_size, "512");
    EXPECT_EQ(parsed->metadata.notes, "Header notes");
    EXPECT_THAT(std::string(bytes->begin(), bytes->end()), testing::HasSubstr("<notes>Document notes</notes>"));
    EXPECT_THAT(parsed->parents, ElementsAre("BASE_XML"));
    auto catalog = session_.Catalog(DefinitionFormat::kEcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->Entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::source, Path("outside.xml"))));
}

TEST_F(DefinitionCatalogSessionIntegration, ImportedFilePreservesTablesAndParentLink)
{
    const std::string xml = "<rom><romid><xmlid>BASE</xmlid></romid><include>PARENT</include>"
                            "<table name=\"Preserved\" address=\"20\" type=\"1D\"/></rom>";
    ASSERT_THAT(files_.Write(Path("source.xml"), std::vector<std::uint8_t>(xml.begin(), xml.end())), IsOk());
    auto input = Header();
    input.include = "PARENT";
    ASSERT_THAT(session_.SubmitImportedDefinition(Path("source.xml"), Path("imported.xml"), input), IsOk());
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "NEW_XML"), Path("imported.xml"));
    auto bytes = files_.Read(Path("imported.xml"));
    ASSERT_THAT(bytes, IsOk());
    auto parsed = fastecu::definition::ParseEcuflashDefinition(*bytes, Path("imported.xml"));
    ASSERT_THAT(parsed, IsOk());
    EXPECT_THAT(parsed->parents, ElementsAre("PARENT"));
    EXPECT_THAT(parsed->maps, ElementsAre(Field(&fastecu::definition::UnresolvedCalibrationMap::name, "Preserved")));
    EXPECT_EQ(parsed->identity.xml_id, "NEW_XML");
}

TEST_F(DefinitionCatalogSessionIntegration, DirectoryChangeDropsDiscoveryAndKeepsSubmissions)
{
    ASSERT_THAT(service_.CreateDefinition(Path("defs/old.xml"), Header("OLD")), IsOk());
    ASSERT_THAT(session_.RefreshIndex(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_THAT(session_.SubmitNewDefinition(Path("outside.xml"), Header("SUBMITTED"), true), IsOk());
    ASSERT_TRUE(QDir().mkpath(root_.filePath("new")));
    ASSERT_THAT(service_.CreateDefinition(Path("new/new.xml"), Header("NEW")), IsOk());
    config_.session.Settings().ecuflash_definition_files_directory = Path("new");
    ASSERT_THAT(session_.RefreshIndex(DefinitionFormat::kEcuFlash), IsOk());
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "OLD"), std::nullopt);
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "NEW"), Path("new/new.xml"));
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "SUBMITTED"), Path("outside.xml"));
}

TEST_F(DefinitionCatalogSessionIntegration, ConflictingAuthoredIdsKeepFirstLookupButRejectFreshCatalog)
{
    ASSERT_THAT(session_.SubmitNewDefinition(Path("first.xml"), Header(), true), IsOk());
    auto conflicting = Header();
    conflicting.ecu_id = "DIFFERENT_ECU";
    ASSERT_THAT(session_.SubmitNewDefinition(Path("second.xml"), conflicting, true), IsOk());
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "NEW_XML"), Path("first.xml"));
    EXPECT_THAT(session_.Catalog(DefinitionFormat::kEcuFlash), IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_THAT(session_.RefreshIndex(DefinitionFormat::kEcuFlash), IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "NEW_XML"), Path("first.xml"));
}

TEST_F(DefinitionCatalogSessionIntegration, OverwritingWithNewIdPreservesOldLookupUntilRefresh)
{
    ASSERT_THAT(session_.SubmitNewDefinition(Path("same.xml"), Header("OLD"), true), IsOk());
    ASSERT_THAT(session_.SubmitNewDefinition(Path("same.xml"), Header("NEW"), true), IsOk());
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "OLD"), Path("same.xml"));
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "NEW"), Path("same.xml"));
    auto catalog = session_.Catalog(DefinitionFormat::kEcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->Entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::definition_id, "NEW")));
    ASSERT_THAT(session_.RefreshIndex(DefinitionFormat::kEcuFlash), IsOk());
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "OLD"), std::nullopt);
}

TEST_F(DefinitionCatalogSessionIntegration, RepeatedSubmittedHandlesAreDiscoveredOnceInSortedOrder)
{
    ASSERT_THAT(session_.SubmitNewDefinition(Path("z.xml"), Header("Z"), true), IsOk());
    ASSERT_THAT(session_.SubmitNewDefinition(Path("a.xml"), Header("A"), true), IsOk());
    ASSERT_THAT(session_.SubmitNewDefinition(Path("z.xml"), Header("Z"), true), IsOk());
    auto catalog = session_.Catalog(DefinitionFormat::kEcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->Entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::source, Path("a.xml")),
                            Field(&fastecu::definition::DefinitionIndexEntry::source, Path("z.xml"))));
}

TEST_F(DefinitionCatalogSessionIntegration, RemovedDiscoveredFileIsNotRetriedByRefresh)
{
    ASSERT_THAT(service_.CreateDefinition(Path("defs/removed.xml"), Header()), IsOk());
    ASSERT_THAT(session_.RefreshIndex(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_TRUE(QFile::remove(root_.filePath("defs/removed.xml")));
    ASSERT_THAT(session_.RefreshIndex(DefinitionFormat::kEcuFlash), IsOk());
    EXPECT_EQ(session_.IndexedSource(DefinitionFormat::kEcuFlash, "NEW_XML"), std::nullopt);
    EXPECT_THAT(config_.events.logs,
                testing::Not(testing::Contains(testing::Pair(fastecu::LogLevel::kError, testing::_))));
}

TEST_F(DefinitionCatalogSessionIntegration, RomOpenFindsAnAuthoredDefinitionOutsideConfiguredDirectory)
{
    auto input = Header();
    input.internal_id_address = 0x10;
    ASSERT_THAT(session_.SubmitNewDefinition(Path("outside.xml"), input, true), IsOk());
    config_.session.Settings().primary_definition_base = "ecuflash";
    config_.session.Settings().use_ecuflash_definitions = "enabled";
    config_.session.Settings().use_romraider_definitions = "disabled";
    std::vector<std::uint8_t> rom(512);
    std::ranges::copy(std::string{"A1B2C3"}, rom.begin() + 0x10);
    ASSERT_THAT(files_.Write(Path("rom.bin"), rom), IsOk());
    fastecu::calibration::RomOpenUseCase opener(session_, service_, files_, file_system_, config_.events,
                                                config_.session);
    auto outcome = opener.OpenFile(Path("rom.bin"));
    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->id, "NEW_XML");
    EXPECT_EQ(outcome->contents.definition->definition.source, Path("outside.xml"));
}

TEST_F(DefinitionCatalogSessionIntegration, RomOpenReportsAnIndexedFileDeletedAfterRefresh)
{
    ASSERT_THAT(service_.CreateDefinition(Path("defs/gone.xml"), Header()), IsOk());
    ASSERT_THAT(session_.RefreshIndex(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_TRUE(QFile::remove(root_.filePath("defs/gone.xml")));
    config_.session.Settings().primary_definition_base = "ecuflash";
    config_.session.Settings().use_ecuflash_definitions = "enabled";
    config_.session.Settings().use_romraider_definitions = "disabled";
    fastecu::calibration::RomOpenUseCase opener(session_, service_, files_, file_system_, config_.events,
                                                config_.session);
    auto outcome =
        opener.AdoptReadImage({.rom = std::vector<std::uint8_t>(512), .filename = "read.bin", .rom_id = "NEW_XML"});
    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(config_.events.notices, testing::Contains("Ecu definitions file: Unable to open ECU definition file " +
                                                          Path("defs/gone.xml") + " for reading"));
}

} // namespace
