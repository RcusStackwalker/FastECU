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

fastecu::definition::DefinitionHeaderInput header(std::string id = "NEW_XML")
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
        ASSERT_THAT(config_.initialize(), IsOk());
        ASSERT_TRUE(QDir().mkpath(root_.filePath("defs")));
        config_.session.settings().ecuflash_definition_files_directory = path("defs");
    }

    std::string path(const char *name) const
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
    auto input = header();
    input.include = "BASE_XML";
    ASSERT_THAT(session_.submit_new_definition(path("outside.xml"), input, true), IsOk());
    auto bytes = files_.read(path("outside.xml"));
    ASSERT_THAT(bytes, IsOk());
    auto parsed = fastecu::definition::parse_ecuflash_definition(*bytes, path("outside.xml"));
    ASSERT_THAT(parsed, IsOk());
    EXPECT_EQ(parsed->identity, (fastecu::definition::RomIdentity{"NEW_XML", "A1B2C3", "ECU-42", 0x1A0}));
    EXPECT_EQ(parsed->metadata.make, "Subaru");
    EXPECT_EQ(parsed->metadata.model, "Legacy");
    EXPECT_EQ(parsed->metadata.flash_method, "proto_a");
    EXPECT_EQ(parsed->metadata.file_size, "512");
    EXPECT_EQ(parsed->metadata.notes, "Header notes");
    EXPECT_THAT(std::string(bytes->begin(), bytes->end()), testing::HasSubstr("<notes>Document notes</notes>"));
    EXPECT_THAT(parsed->parents, ElementsAre("BASE_XML"));
    auto catalog = session_.catalog(DefinitionFormat::kEcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::source, path("outside.xml"))));
}

TEST_F(DefinitionCatalogSessionIntegration, ImportedFilePreservesTablesAndParentLink)
{
    const std::string xml = "<rom><romid><xmlid>BASE</xmlid></romid><include>PARENT</include>"
                            "<table name=\"Preserved\" address=\"20\" type=\"1D\"/></rom>";
    ASSERT_THAT(files_.write(path("source.xml"), std::vector<std::uint8_t>(xml.begin(), xml.end())), IsOk());
    auto input = header();
    input.include = "PARENT";
    ASSERT_THAT(session_.submit_imported_definition(path("source.xml"), path("imported.xml"), input), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), path("imported.xml"));
    auto bytes = files_.read(path("imported.xml"));
    ASSERT_THAT(bytes, IsOk());
    auto parsed = fastecu::definition::parse_ecuflash_definition(*bytes, path("imported.xml"));
    ASSERT_THAT(parsed, IsOk());
    EXPECT_THAT(parsed->parents, ElementsAre("PARENT"));
    EXPECT_THAT(parsed->maps, ElementsAre(Field(&fastecu::definition::UnresolvedCalibrationMap::name, "Preserved")));
    EXPECT_EQ(parsed->identity.xml_id, "NEW_XML");
}

TEST_F(DefinitionCatalogSessionIntegration, DirectoryChangeDropsDiscoveryAndKeepsSubmissions)
{
    ASSERT_THAT(service_.create_definition(path("defs/old.xml"), header("OLD")), IsOk());
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_THAT(session_.submit_new_definition(path("outside.xml"), header("SUBMITTED"), true), IsOk());
    ASSERT_TRUE(QDir().mkpath(root_.filePath("new")));
    ASSERT_THAT(service_.create_definition(path("new/new.xml"), header("NEW")), IsOk());
    config_.session.settings().ecuflash_definition_files_directory = path("new");
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), std::nullopt);
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW"), path("new/new.xml"));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "SUBMITTED"), path("outside.xml"));
}

TEST_F(DefinitionCatalogSessionIntegration, ConflictingAuthoredIdsKeepFirstLookupButRejectFreshCatalog)
{
    ASSERT_THAT(session_.submit_new_definition(path("first.xml"), header(), true), IsOk());
    auto conflicting = header();
    conflicting.ecu_id = "DIFFERENT_ECU";
    ASSERT_THAT(session_.submit_new_definition(path("second.xml"), conflicting, true), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), path("first.xml"));
    EXPECT_THAT(session_.catalog(DefinitionFormat::kEcuFlash), IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), path("first.xml"));
}

TEST_F(DefinitionCatalogSessionIntegration, OverwritingWithNewIdPreservesOldLookupUntilRefresh)
{
    ASSERT_THAT(session_.submit_new_definition(path("same.xml"), header("OLD"), true), IsOk());
    ASSERT_THAT(session_.submit_new_definition(path("same.xml"), header("NEW"), true), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), path("same.xml"));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW"), path("same.xml"));
    auto catalog = session_.catalog(DefinitionFormat::kEcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::definition_id, "NEW")));
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), std::nullopt);
}

TEST_F(DefinitionCatalogSessionIntegration, RepeatedSubmittedHandlesAreDiscoveredOnceInSortedOrder)
{
    ASSERT_THAT(session_.submit_new_definition(path("z.xml"), header("Z"), true), IsOk());
    ASSERT_THAT(session_.submit_new_definition(path("a.xml"), header("A"), true), IsOk());
    ASSERT_THAT(session_.submit_new_definition(path("z.xml"), header("Z"), true), IsOk());
    auto catalog = session_.catalog(DefinitionFormat::kEcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::source, path("a.xml")),
                            Field(&fastecu::definition::DefinitionIndexEntry::source, path("z.xml"))));
}

TEST_F(DefinitionCatalogSessionIntegration, RemovedDiscoveredFileIsNotRetriedByRefresh)
{
    ASSERT_THAT(service_.create_definition(path("defs/removed.xml"), header()), IsOk());
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_TRUE(QFile::remove(root_.filePath("defs/removed.xml")));
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), std::nullopt);
    EXPECT_THAT(config_.events.logs,
                testing::Not(testing::Contains(testing::Pair(fastecu::LogLevel::kError, testing::_))));
}

TEST_F(DefinitionCatalogSessionIntegration, RomOpenFindsAnAuthoredDefinitionOutsideConfiguredDirectory)
{
    auto input = header();
    input.internal_id_address = 0x10;
    ASSERT_THAT(session_.submit_new_definition(path("outside.xml"), input, true), IsOk());
    config_.session.settings().primary_definition_base = "ecuflash";
    config_.session.settings().use_ecuflash_definitions = "enabled";
    config_.session.settings().use_romraider_definitions = "disabled";
    std::vector<std::uint8_t> rom(512);
    std::ranges::copy(std::string{"A1B2C3"}, rom.begin() + 0x10);
    ASSERT_THAT(files_.write(path("rom.bin"), rom), IsOk());
    fastecu::calibration::RomOpenUseCase opener(session_, service_, files_, file_system_, config_.events,
                                                config_.session);
    auto outcome = opener.open_file(path("rom.bin"));
    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->id, "NEW_XML");
    EXPECT_EQ(outcome->contents.definition->definition.source, path("outside.xml"));
}

TEST_F(DefinitionCatalogSessionIntegration, RomOpenReportsAnIndexedFileDeletedAfterRefresh)
{
    ASSERT_THAT(service_.create_definition(path("defs/gone.xml"), header()), IsOk());
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_TRUE(QFile::remove(root_.filePath("defs/gone.xml")));
    config_.session.settings().primary_definition_base = "ecuflash";
    config_.session.settings().use_ecuflash_definitions = "enabled";
    config_.session.settings().use_romraider_definitions = "disabled";
    fastecu::calibration::RomOpenUseCase opener(session_, service_, files_, file_system_, config_.events,
                                                config_.session);
    auto outcome =
        opener.adopt_read_image({.rom = std::vector<std::uint8_t>(512), .filename = "read.bin", .rom_id = "NEW_XML"});
    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(config_.events.notices, testing::Contains("Ecu definitions file: Unable to open ECU definition file " +
                                                          path("defs/gone.xml") + " for reading"));
}

} // namespace
