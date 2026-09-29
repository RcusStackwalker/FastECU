// Catalog characterization migrated from src/backend/definitions/file_actions.cpp.
#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/definition/definition_catalog_session.h"

namespace
{
using fastecu::definition::DefinitionFormat;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using testing::ElementsAre;
using testing::Field;
using testing::HasSubstr;

class DefinitionCatalogSession : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(config.initialize(), IsOk());
        config.session.settings().ecuflash_definition_files_directory = "/defs";
        config.file_system.directory_entries["/defs"] = {};
    }

    void put_romraider(const std::string& source, std::string_view id)
    {
        config.put(source, std::format("<roms><rom><romid><xmlid>{}</xmlid></romid></rom></roms>", id));
    }

    void put_ecuflash(const std::string& directory, const std::string& name, std::string_view id)
    {
        config.file_system.directory_entries[directory] = {{.name = name, .is_directory = false}};
        config.put(directory + "/" + name, std::format("<rom><romid><xmlid>{}</xmlid></romid></rom>", id));
    }

    fastecu::config::testing::ConfigSessionFixture config;
    fastecu::InMemoryAtomicFileWriter writer;
    fastecu::definition::DefinitionService service{config.file_system, config.file_repository, writer};
    fastecu::desktop::definition::DefinitionCatalogSession session{service, config.session, config.file_system,
                                                                   config.events};
};

TEST_F(DefinitionCatalogSession, RetainsConfiguredRomraiderOrderAndFormatSpecificLookup)
{
    put_romraider("b.xml", "ZZZ_FIRST");
    put_romraider("a.xml", "AAA_SECOND");
    config.session.settings().romraider_definition_files = {"b.xml", "a.xml"};
    ASSERT_THAT(session.refresh_index(DefinitionFormat::RomRaider), IsOk());
    auto catalog = session.catalog(DefinitionFormat::RomRaider);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->entries(), ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::source, "b.xml"),
                                                Field(&fastecu::definition::DefinitionIndexEntry::source, "a.xml")));
    EXPECT_EQ(session.indexed_source(DefinitionFormat::RomRaider, "ZZZ_FIRST"), "b.xml");
    EXPECT_EQ(session.indexed_source(DefinitionFormat::RomRaider, "AAA_SECOND"), "a.xml");
    EXPECT_EQ(session.indexed_source(DefinitionFormat::EcuFlash, "ZZZ_FIRST"), std::nullopt);
    EXPECT_EQ(session.indexed_source(DefinitionFormat::RomRaider, "UNKNOWN"), std::nullopt);
}

TEST_F(DefinitionCatalogSession, FreshCatalogDoesNotReplaceRetainedLookup)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    ASSERT_THAT(session.refresh_index(DefinitionFormat::EcuFlash), IsOk());
    put_ecuflash("/defs", "new.xml", "NEW");
    auto catalog = session.catalog(DefinitionFormat::EcuFlash);
    ASSERT_THAT(catalog, IsOk());
    EXPECT_THAT(catalog->entries(),
                ElementsAre(Field(&fastecu::definition::DefinitionIndexEntry::definition_id, "NEW")));
    EXPECT_EQ(session.indexed_source(DefinitionFormat::EcuFlash, "OLD"), "/defs/old.xml");
    EXPECT_EQ(session.indexed_source(DefinitionFormat::EcuFlash, "NEW"), std::nullopt);
}

TEST_F(DefinitionCatalogSession, EmptySourcesPreserveBothIndexes)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    put_romraider("rr.xml", "RR");
    config.session.settings().romraider_definition_files = {"rr.xml"};
    ASSERT_THAT(session.refresh_index(DefinitionFormat::EcuFlash), IsOk());
    ASSERT_THAT(session.refresh_index(DefinitionFormat::RomRaider), IsOk());
    config.session.settings().ecuflash_definition_files_directory.clear();
    config.session.settings().romraider_definition_files.clear();
    ASSERT_THAT(session.refresh_index(DefinitionFormat::EcuFlash), IsOk());
    ASSERT_THAT(session.refresh_index(DefinitionFormat::RomRaider), IsOk());
    EXPECT_EQ(session.indexed_source(DefinitionFormat::EcuFlash, "OLD"), "/defs/old.xml");
    EXPECT_EQ(session.indexed_source(DefinitionFormat::RomRaider, "RR"), "rr.xml");
}

TEST_F(DefinitionCatalogSession, FailedScanPreservesIndexAndReportsError)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    ASSERT_THAT(session.refresh_index(DefinitionFormat::EcuFlash), IsOk());
    config.file_system.list_directory_errors["/defs"] = {fastecu::ErrorKind::Disconnected, "directory unavailable"};
    EXPECT_THAT(session.refresh_index(DefinitionFormat::EcuFlash),
                IsErrWith(fastecu::ErrorKind::Disconnected, HasSubstr("directory unavailable")));
    EXPECT_EQ(session.indexed_source(DefinitionFormat::EcuFlash, "OLD"), "/defs/old.xml");
    EXPECT_THAT(config.events.logs,
                testing::Contains(testing::Pair(
                    fastecu::LogLevel::Error,
                    "Unable to build EcuFlash definition catalog [Disconnected]: directory unavailable")));
}

TEST_F(DefinitionCatalogSession, MalformedFilesSuccessfullyReplaceIndexesWithEmpty)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    put_romraider("rr.xml", "RR");
    config.session.settings().romraider_definition_files = {"rr.xml"};
    ASSERT_THAT(session.refresh_index(DefinitionFormat::EcuFlash), IsOk());
    ASSERT_THAT(session.refresh_index(DefinitionFormat::RomRaider), IsOk());
    config.put("/defs/old.xml", "<rom><romid>");
    config.put("rr.xml", "<roms><rom>");
    ASSERT_THAT(session.refresh_index(DefinitionFormat::EcuFlash), IsOk());
    ASSERT_THAT(session.refresh_index(DefinitionFormat::RomRaider), IsOk());
    EXPECT_EQ(session.indexed_source(DefinitionFormat::EcuFlash, "OLD"), std::nullopt);
    EXPECT_EQ(session.indexed_source(DefinitionFormat::RomRaider, "RR"), std::nullopt);
}

TEST_F(DefinitionCatalogSession, DirectoryChangeDropsDiscoveryWithoutChangingOtherFormat)
{
    put_ecuflash("/defs", "old.xml", "OLD");
    put_romraider("rr.xml", "RR");
    config.session.settings().romraider_definition_files = {"rr.xml"};
    ASSERT_THAT(session.refresh_index(DefinitionFormat::EcuFlash), IsOk());
    ASSERT_THAT(session.refresh_index(DefinitionFormat::RomRaider), IsOk());
    put_ecuflash("/new", "new.xml", "NEW");
    config.session.settings().ecuflash_definition_files_directory = "/new";
    ASSERT_THAT(session.refresh_index(DefinitionFormat::EcuFlash), IsOk());
    EXPECT_EQ(session.indexed_source(DefinitionFormat::EcuFlash, "OLD"), std::nullopt);
    EXPECT_EQ(session.indexed_source(DefinitionFormat::EcuFlash, "NEW"), "/new/new.xml");
    EXPECT_EQ(session.indexed_source(DefinitionFormat::RomRaider, "RR"), "rr.xml");
}
} // namespace
