// Catalog characterization migrated from src/backend/definitions/file_actions.cpp.
#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/definition/definition_catalog_session.h"

namespace
{
using fastecu::definition::DefinitionFormat;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using testing::ElementsAre;
using testing::Field;
using testing::HasSubstr;

fastecu::definition::DefinitionHeaderInput header()
{
    return {.xml_id = "NEW_XML", .internal_id = "A1B2C3", .ecu_id = "ECU-42", .internal_id_address = 0x1A0};
}

class DefinitionCatalogSession : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(config_.initialize(), IsOk());
        config_.session.settings().ecuflash_definition_files_directory = "/defs";
        config_.file_system.directory_entries["/defs"] = {};
    }

    void put_romraider(const std::string& source, std::string_view id)
    {
        config_.put(source, std::format("<roms><rom><romid><xmlid>{}</xmlid></romid></rom></roms>", id));
    }

    void put_ecuflash(const std::string& directory, const std::string& name, std::string_view id)
    {
        config_.file_system.directory_entries[directory] = {{.name = name, .is_directory = false}};
        config_.put(directory + "/" + name, std::format("<rom><romid><xmlid>{}</xmlid></romid></rom>", id));
    }

    fastecu::config::testing::ConfigSessionFixture config_;
    fastecu::InMemoryAtomicFileWriter writer_;
    fastecu::definition::DefinitionService service_{config_.file_system, config_.file_repository, writer_};
    fastecu::definition::DefinitionCatalogSession session_{service_, config_.session, config_.file_system,
                                                           config_.events};
};

TEST_F(DefinitionCatalogSession, RetainsConfiguredRomraiderOrderAndFormatSpecificLookup)
{
    put_romraider("b.xml", "ZZZ_FIRST");
    put_romraider("a.xml", "AAA_SECOND");
    config_.session.settings().romraider_definition_files = {"b.xml", "a.xml"};
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kRomRaider), IsOk());
    auto catalog = session_.catalog(DefinitionFormat::kRomRaider);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->entries(), ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::source, "b.xml"),
                                                Field(&fastecu::definition::DefinitionIndexEntry::source, "a.xml")));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kRomRaider, "ZZZ_FIRST"), "b.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kRomRaider, "AAA_SECOND"), "a.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "ZZZ_FIRST"), std::nullopt);
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kRomRaider, "UNKNOWN"), std::nullopt);
}

TEST_F(DefinitionCatalogSession, FreshCatalogDoesNotReplaceRetainedLookup)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    put_ecuflash("/defs", "new.xml", "NEW");
    auto catalog = session_.catalog(DefinitionFormat::kEcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::definition_id, "NEW")));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), "/defs/old.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW"), std::nullopt);
}

TEST_F(DefinitionCatalogSession, EmptySourcesPreserveBothIndexes)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    put_romraider("rr.xml", "RR");
    config_.session.settings().romraider_definition_files = {"rr.xml"};
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kRomRaider), IsOk());
    config_.session.settings().ecuflash_definition_files_directory.clear();
    config_.session.settings().romraider_definition_files.clear();
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kRomRaider), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), "/defs/old.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kRomRaider, "RR"), "rr.xml");
}

TEST_F(DefinitionCatalogSession, FailedScanPreservesIndexAndReportsError)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    config_.file_system.list_directory_errors["/defs"] = {fastecu::ErrorKind::kDisconnected, "directory unavailable"};
    EXPECT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash),
                IsErrWith(fastecu::ErrorKind::kDisconnected, HasSubstr("directory unavailable")));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), "/defs/old.xml");
    EXPECT_THAT(config_.events.logs,
                testing::Contains(testing::Pair(
                    fastecu::LogLevel::kError,
                    "Unable to build EcuFlash definition catalog [Disconnected]: directory unavailable")));
}

TEST_F(DefinitionCatalogSession, MalformedFilesSuccessfullyReplaceIndexesWithEmpty)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    put_romraider("rr.xml", "RR");
    config_.session.settings().romraider_definition_files = {"rr.xml"};
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kRomRaider), IsOk());
    config_.put("/defs/old.xml", "<rom><romid>");
    config_.put("rr.xml", "<roms><rom>");
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kRomRaider), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), std::nullopt);
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kRomRaider, "RR"), std::nullopt);
}

TEST_F(DefinitionCatalogSession, DirectoryChangeDropsDiscoveryWithoutChangingOtherFormat)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    put_romraider("rr.xml", "RR");
    config_.session.settings().romraider_definition_files = {"rr.xml"};
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kRomRaider), IsOk());
    put_ecuflash("/new", "new.xml", "NEW");
    config_.session.settings().ecuflash_definition_files_directory = "/new";
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), std::nullopt);
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW"), "/new/new.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kRomRaider, "RR"), "rr.xml");
}

TEST_F(DefinitionCatalogSession, RemovedDiscoveredFilesAreNotReadAgain)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    config_.file_system.directory_entries["/defs"].clear();
    config_.file_repository.files.erase("/defs/old.xml");
    config_.file_repository.read_handles.clear();
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    EXPECT_THAT(config_.file_repository.read_handles, testing::IsEmpty());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), std::nullopt);
}

TEST_F(DefinitionCatalogSession, FailedRomraiderScanPreservesLookupAndNotifiesFirstMissingFile)
{
    put_romraider("old.xml", "OLD");
    config_.session.settings().romraider_definition_files = {"old.xml"};
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kRomRaider), IsOk());
    config_.put("a.xml", "<roms><rom><romid><xmlid>DUPLICATE</xmlid><ecuid>A</ecuid></romid></rom></roms>");
    config_.put("b.xml", "<roms><rom><romid><xmlid>DUPLICATE</xmlid><ecuid>B</ecuid></romid></rom></roms>");
    config_.session.settings().romraider_definition_files = {"missing.xml", "a.xml", "b.xml"};
    EXPECT_THAT(session_.refresh_index(DefinitionFormat::kRomRaider),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kRomRaider, "OLD"), "old.xml");
    EXPECT_THAT(config_.events.notices,
                ElementsAre("Ecu definition file: Unable to open romraider definition file missing.xml for reading"));
}
TEST_F(DefinitionCatalogSession, SuccessfulCreateImmediatelyRegistersSource)
{
    ASSERT_THAT(session_.submit_new_definition("outside.xml", header(), true), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), "outside.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kRomRaider, "NEW_XML"), std::nullopt);
}

TEST_F(DefinitionCatalogSession, FailedWritesPreserveLookupAndLogExactError)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    ASSERT_THAT(session_.refresh_index(DefinitionFormat::kEcuFlash), IsOk());
    writer_.replace_error = {fastecu::ErrorKind::kDisconnected, "atomic destination unavailable"};
    EXPECT_THAT(session_.submit_new_definition("unavailable.xml", header(), true),
                IsErrWith(fastecu::ErrorKind::kDisconnected, "atomic destination unavailable"));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "OLD"), "/defs/old.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), std::nullopt);
    EXPECT_THAT(
        config_.events.logs,
        testing::Contains(testing::Pair(fastecu::LogLevel::kError,
                                        "Unable to create definition [Disconnected]: atomic destination unavailable")));
    auto catalog = session_.catalog(DefinitionFormat::kEcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::definition_id, "OLD")));
}

TEST_F(DefinitionCatalogSession, InvalidHeadersDoNotWriteOrRegister)
{
    auto input = header();
    input.xml_id.clear();
    EXPECT_THAT(session_.submit_new_definition("invalid.xml", input, true),
                IsErrWith(fastecu::ErrorKind::kInvalidConfig, "definition XML ID is required"));
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, ""), std::nullopt);
    config_.put("base.xml", "<rom><romid><xmlid>BASE</xmlid></romid></rom>");
    EXPECT_THAT(session_.submit_imported_definition("base.xml", "invalid-import.xml", input),
                IsErrWith(fastecu::ErrorKind::kInvalidConfig, "definition XML ID is required"));
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
}

TEST_F(DefinitionCatalogSession, OverwriteRequiresExplicitAuthorization)
{
    config_.file_system.files["existing.xml"] = {};
    EXPECT_THAT(session_.submit_new_definition("existing.xml", header(), false),
                fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_THAT(writer_.replace_calls, testing::IsEmpty());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), std::nullopt);
    ASSERT_THAT(session_.submit_new_definition("existing.xml", header(), true), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), "existing.xml");
}

TEST_F(DefinitionCatalogSession, ImportRegistersOnlyAfterSuccessfulWrite)
{
    config_.put("base.xml", "<rom><romid><xmlid>BASE</xmlid></romid><table name=\"Preserved\"/></rom>");
    writer_.replace_error = {fastecu::ErrorKind::kDisconnected, "atomic destination unavailable"};
    EXPECT_THAT(session_.submit_imported_definition("base.xml", "imported.xml", header()),
                IsErrWith(fastecu::ErrorKind::kDisconnected, "atomic destination unavailable"));
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), std::nullopt);
    EXPECT_THAT(
        config_.events.logs,
        testing::Contains(testing::Pair(fastecu::LogLevel::kError,
                                        "Unable to import definition [Disconnected]: atomic destination unavailable")));
    writer_.replace_error.reset();
    ASSERT_THAT(session_.submit_imported_definition("base.xml", "imported.xml", header()), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "NEW_XML"), "imported.xml");
}

TEST_F(DefinitionCatalogSession, RegistersCanonicalXmlIdForDirectCreateAndImport)
{
    auto input = header();
    input.xml_id = "\xc2\xa0"
                   "CREATED\xe3\x80\x80";
    ASSERT_THAT(session_.submit_new_definition("created.xml", input, true), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "CREATED"), "created.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, input.xml_id), std::nullopt);
    config_.put("base.xml", "<rom><romid><xmlid>BASE</xmlid></romid></rom>");
    input.xml_id = "\xc2\xa0"
                   "IMPORTED\xe3\x80\x80";
    ASSERT_THAT(session_.submit_imported_definition("base.xml", "imported.xml", input), IsOk());
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, "IMPORTED"), "imported.xml");
    EXPECT_EQ(session_.indexed_source(DefinitionFormat::kEcuFlash, input.xml_id), std::nullopt);
}
} // namespace
