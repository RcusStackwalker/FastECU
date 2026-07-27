#include "src/backend/definition/definition_service.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{

std::vector<std::uint8_t> bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

std::vector<std::uint8_t> romraider_xml(std::string_view id)
{
    return bytes(
        "<roms><rom><romid><xmlid>" + std::string(id) +
        "</xmlid></romid></rom></roms>");
}

std::vector<std::uint8_t> ecuflash_xml(std::string_view id)
{
    return bytes(
        "<rom><romid><xmlid>" + std::string(id) + "</xmlid></romid></rom>");
}

DefinitionIndexEntry index_entry(
    std::string id,
    std::string internal_id,
    std::optional<std::uint64_t> address,
    IdEncoding encoding = IdEncoding::Ascii)
{
    return DefinitionIndexEntry{
        .format = DefinitionFormat::RomRaider,
        .definition_id = std::move(id),
        .internal_id = std::move(internal_id),
        .internal_id_address = address,
        .internal_id_encoding = encoding,
        .source = "definitions.xml",
    };
}

DefinitionIndexEntry load_entry(
    DefinitionFormat format,
    std::string id,
    std::string source)
{
    DefinitionIndexEntry entry = index_entry(std::move(id), "", std::nullopt);
    entry.format = format;
    entry.source = std::move(source);
    return entry;
}

class FakeFileSystem : public IFileSystem
{
  public:
    bool exists(std::string_view path) override
    {
        return directories.contains(std::string(path));
    }

    Status create_directory(std::string_view) override
    {
        return {};
    }

    Status copy_file(std::string_view, std::string_view, bool) override
    {
        return {};
    }

    Status remove_file(std::string_view) override
    {
        return {};
    }

    Result<std::vector<DirEntry>> list_directory(std::string_view path) override
    {
        const std::string key(path);
        if (auto error = list_errors.find(key); error != list_errors.end())
        {
            return std::unexpected(error->second);
        }
        if (auto directory = directories.find(key); directory != directories.end())
        {
            return directory->second;
        }
        return fail(ErrorKind::InvalidConfig, "unknown test directory: " + key);
    }

    std::map<std::string, std::vector<DirEntry>> directories;
    std::map<std::string, Error> list_errors;
};

class FakeFileRepository : public IFileRepository
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view handle) override
    {
        const std::string key(handle);
        ++read_counts[key];
        if (auto error = read_errors.find(key); error != read_errors.end())
        {
            return std::unexpected(error->second);
        }
        if (auto file = files.find(key); file != files.end())
        {
            return file->second;
        }
        return fail(ErrorKind::InvalidConfig, "unknown test file: " + key);
    }

    Status write(std::string_view, std::span<const std::uint8_t>) override
    {
        return {};
    }

    std::map<std::string, std::vector<std::uint8_t>> files;
    std::map<std::string, Error> read_errors;
    std::map<std::string, int> read_counts;
};

class FakeAtomicFileWriter : public IAtomicFileWriter
{
  public:
    Status replace(std::string_view, std::span<const std::uint8_t>) override
    {
        return {};
    }
};

class DefinitionServiceTest : public ::testing::Test
{
  protected:
    FakeFileSystem file_system;
    FakeFileRepository repository;
    FakeAtomicFileWriter writer;
    DefinitionService service{file_system, repository, writer};
};

TEST_F(DefinitionServiceTest, DiscoversEcuFlashXmlRecursivelyInLexicalFullPathOrder)
{
    file_system.directories["defs"] = {
        DirEntry{.name = "z.xml", .is_directory = false},
        DirEntry{.name = "nested", .is_directory = true},
        DirEntry{.name = "notes.txt", .is_directory = false},
        DirEntry{.name = "pretend.xml", .is_directory = true},
    };
    file_system.directories["defs/nested"] = {
        DirEntry{.name = "b.XML", .is_directory = false},
        DirEntry{.name = "a.Xml", .is_directory = false},
    };
    file_system.directories["defs/pretend.xml"] = {
        DirEntry{.name = "ignored.bin", .is_directory = false},
    };
    repository.files["defs/z.xml"] = ecuflash_xml("Z");
    repository.files["defs/nested/b.XML"] = ecuflash_xml("B");
    repository.files["defs/nested/a.Xml"] = ecuflash_xml("A");

    auto result = service.build_ecuflash_catalog("defs");

    ASSERT_TRUE(result);
    ASSERT_EQ(result->entries().size(), 3U);
    EXPECT_EQ(result->entries()[0].definition_id, "A");
    EXPECT_EQ(result->entries()[1].definition_id, "B");
    EXPECT_EQ(result->entries()[2].definition_id, "Z");
    EXPECT_EQ(repository.read_counts["defs/nested/a.Xml"], 1);
    EXPECT_EQ(repository.read_counts["defs/nested/b.XML"], 1);
    EXPECT_EQ(repository.read_counts["defs/z.xml"], 1);
    EXPECT_FALSE(repository.read_counts.contains("defs/notes.txt"));
    EXPECT_FALSE(repository.read_counts.contains("defs/pretend.xml"));
}

TEST_F(DefinitionServiceTest, PreservesConfiguredRomRaiderHandleOrderingAndReadsEachOnce)
{
    repository.files["second.xml"] = romraider_xml("SECOND");
    repository.files["first.xml"] = romraider_xml("FIRST");
    const std::vector<std::string> handles{"second.xml", "first.xml"};

    auto result = service.build_romraider_catalog(handles);

    ASSERT_TRUE(result);
    ASSERT_EQ(result->entries().size(), 2U);
    EXPECT_EQ(result->entries()[0].definition_id, "SECOND");
    EXPECT_EQ(result->entries()[1].definition_id, "FIRST");
    EXPECT_EQ(repository.read_counts["second.xml"], 1);
    EXPECT_EQ(repository.read_counts["first.xml"], 1);
}

TEST_F(DefinitionServiceTest, PropagatesDiscoveryFailureWithoutReadingFiles)
{
    file_system.list_errors["defs"] =
        Error{ErrorKind::Disconnected, "catalog directory unavailable"};

    auto result = service.build_ecuflash_catalog("defs");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(),
              (Error{ErrorKind::Disconnected, "catalog directory unavailable"}));
    EXPECT_TRUE(repository.read_counts.empty());
}

TEST_F(DefinitionServiceTest, PropagatesRepositoryFailure)
{
    const std::vector<std::string> handles{"bad.xml"};
    repository.read_errors["bad.xml"] = Error{ErrorKind::Disconnected, "read failed"};

    auto result = service.build_romraider_catalog(handles);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), (Error{ErrorKind::Disconnected, "read failed"}));
    EXPECT_EQ(repository.read_counts["bad.xml"], 1);
}

TEST_F(DefinitionServiceTest, PropagatesParseFailureAfterOneRead)
{
    file_system.directories["defs"] = {
        DirEntry{.name = "bad.xml", .is_directory = false},
    };
    repository.files["defs/bad.xml"] = bytes("<not-rom/>");

    auto result = service.build_ecuflash_catalog("defs");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_counts["defs/bad.xml"], 1);
}

TEST_F(DefinitionServiceTest, MatchesAsciiIdentifierEndingExactlyAtRomEnd)
{
    auto catalog = DefinitionCatalog::create(
        {index_entry("EXACT", "AB", 1U)});
    ASSERT_TRUE(catalog);
    const std::vector<std::uint8_t> rom{'x', 'A', 'B'};

    auto result = service.match_rom(*catalog, rom);

    ASSERT_TRUE(result);
    EXPECT_EQ(result->definition_id, "EXACT");
}

TEST_F(DefinitionServiceTest, RejectsIdentifierWhenRomIsOneByteShort)
{
    auto catalog = DefinitionCatalog::create(
        {index_entry("SHORT", "AB", 1U)});
    ASSERT_TRUE(catalog);
    const std::vector<std::uint8_t> rom{'x', 'A'};

    auto result = service.match_rom(*catalog, rom);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST_F(DefinitionServiceTest, MatchesUpperAndLowerCaseHexText)
{
    auto lower_catalog = DefinitionCatalog::create(
        {index_entry("LOWER", "ab10", 0U, IdEncoding::Hex)});
    auto upper_catalog = DefinitionCatalog::create(
        {index_entry("UPPER", "AB10", 0U, IdEncoding::Hex)});
    ASSERT_TRUE(lower_catalog);
    ASSERT_TRUE(upper_catalog);
    const std::vector<std::uint8_t> rom{0xAB, 0x10};

    auto lower = service.match_rom(*lower_catalog, rom);
    auto upper = service.match_rom(*upper_catalog, rom);

    ASSERT_TRUE(lower);
    ASSERT_TRUE(upper);
    EXPECT_EQ(lower->definition_id, "LOWER");
    EXPECT_EQ(upper->definition_id, "UPPER");
}

TEST_F(DefinitionServiceTest, RejectsOddLengthHexIdentifierBeforeLaterMatch)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("ODD", "ABC", 0U, IdEncoding::Hex),
        index_entry("VALID", "AB", 0U, IdEncoding::Hex),
    });
    ASSERT_TRUE(catalog);
    const std::vector<std::uint8_t> rom{0xAB};

    auto result = service.match_rom(*catalog, rom);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("ODD"), std::string::npos);
    EXPECT_NE(result.error().detail.find("definitions.xml"), std::string::npos);
    EXPECT_NE(result.error().detail.find("odd"), std::string::npos);
}

TEST_F(DefinitionServiceTest, RejectsInvalidHexDigitBeforeLaterMatch)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("INVALID_HEX", "AG", 0U, IdEncoding::Hex),
        index_entry("VALID", "41", 0U, IdEncoding::Hex),
    });
    ASSERT_TRUE(catalog);
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service.match_rom(*catalog, rom);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("INVALID_HEX"), std::string::npos);
    EXPECT_NE(result.error().detail.find("hexadecimal"), std::string::npos);
}

TEST_F(DefinitionServiceTest, RejectsMissingAddressBeforeLaterMatch)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("NO_ADDRESS", "A", std::nullopt),
        index_entry("VALID", "A", 0U),
    });
    ASSERT_TRUE(catalog);
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service.match_rom(*catalog, rom);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("NO_ADDRESS"), std::string::npos);
    EXPECT_NE(result.error().detail.find("address"), std::string::npos);
}

TEST_F(DefinitionServiceTest, RejectsAddressBeyondRomBoundsBeforeLaterMatch)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("OUT_OF_RANGE", "A", 2U),
        index_entry("VALID", "A", 0U),
    });
    ASSERT_TRUE(catalog);
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service.match_rom(*catalog, rom);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("OUT_OF_RANGE"), std::string::npos);
    EXPECT_NE(result.error().detail.find("address"), std::string::npos);
}

TEST_F(DefinitionServiceTest, ReturnsFirstCatalogEntryWhenMatchesAreAmbiguous)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("FIRST", "A", 0U),
        index_entry("SECOND", "A", 0U),
    });
    ASSERT_TRUE(catalog);
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service.match_rom(*catalog, rom);

    ASSERT_TRUE(result);
    EXPECT_EQ(result->definition_id, "FIRST");
}

TEST_F(DefinitionServiceTest, EmptyIdentifierDoesNotMatch)
{
    auto catalog = DefinitionCatalog::create(
        {index_entry("EMPTY", "", 0U)});
    ASSERT_TRUE(catalog);

    auto result = service.match_rom(*catalog, {});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST_F(DefinitionServiceTest, ReturnsInvalidConfigWhenNoIdentifierMatches)
{
    auto catalog = DefinitionCatalog::create(
        {index_entry("OTHER", "B", 0U)});
    ASSERT_TRUE(catalog);
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service.match_rom(*catalog, rom);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST_F(DefinitionServiceTest, LoadsAndResolvesRomRaiderChildAndBaseFiles)
{
    repository.files["child.xml"] = bytes(R"xml(
      <roms><rom base="BASE"><romid><xmlid>CHILD</xmlid></romid></rom></roms>)xml");
    repository.files["base.xml"] = romraider_xml("BASE");
    auto catalog = DefinitionCatalog::create({
        load_entry(DefinitionFormat::RomRaider, "CHILD", "child.xml"),
        load_entry(DefinitionFormat::RomRaider, "BASE", "base.xml"),
    });
    ASSERT_TRUE(catalog);

    auto result = service.load(*catalog, DefinitionFormat::RomRaider, "CHILD");

    ASSERT_TRUE(result);
    EXPECT_EQ(result->identity.xml_id, "CHILD");
    EXPECT_EQ(result->resolved_sources,
              (std::vector<std::string>{"base.xml", "child.xml"}));
    EXPECT_EQ(repository.read_counts["child.xml"], 1);
    EXPECT_EQ(repository.read_counts["base.xml"], 1);
}

TEST_F(DefinitionServiceTest, MemoizesRepeatedEcuFlashParentOncePerLoadCall)
{
    repository.files["root.xml"] = bytes(R"xml(
      <rom><romid><xmlid>ROOT</xmlid></romid>
      <include>LEFT</include><include>RIGHT</include></rom>)xml");
    repository.files["left.xml"] = bytes(R"xml(
      <rom><romid><xmlid>LEFT</xmlid></romid><include>BASE</include></rom>)xml");
    repository.files["right.xml"] = bytes(R"xml(
      <rom><romid><xmlid>RIGHT</xmlid></romid><include>BASE</include></rom>)xml");
    repository.files["base.xml"] = ecuflash_xml("BASE");
    auto catalog = DefinitionCatalog::create({
        load_entry(DefinitionFormat::EcuFlash, "ROOT", "root.xml"),
        load_entry(DefinitionFormat::EcuFlash, "LEFT", "left.xml"),
        load_entry(DefinitionFormat::EcuFlash, "RIGHT", "right.xml"),
        load_entry(DefinitionFormat::EcuFlash, "BASE", "base.xml"),
    });
    ASSERT_TRUE(catalog);

    auto first = service.load(*catalog, DefinitionFormat::EcuFlash, "ROOT");
    auto second = service.load(*catalog, DefinitionFormat::EcuFlash, "ROOT");

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->identity.xml_id, "ROOT");
    EXPECT_EQ(second->identity.xml_id, "ROOT");
    EXPECT_EQ(repository.read_counts["root.xml"], 2);
    EXPECT_EQ(repository.read_counts["left.xml"], 2);
    EXPECT_EQ(repository.read_counts["right.xml"], 2);
    EXPECT_EQ(repository.read_counts["base.xml"], 2);
}

TEST_F(DefinitionServiceTest, LoadPropagatesRepositoryFailure)
{
    auto catalog = DefinitionCatalog::create(
        {load_entry(DefinitionFormat::EcuFlash, "BROKEN", "broken.xml")});
    ASSERT_TRUE(catalog);
    repository.read_errors["broken.xml"] =
        Error{ErrorKind::Disconnected, "definition read failed"};

    auto result = service.load(*catalog, DefinitionFormat::EcuFlash, "BROKEN");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(),
              (Error{ErrorKind::Disconnected, "definition read failed"}));
}

TEST_F(DefinitionServiceTest, LoadPropagatesParentRepositoryFailureUnchanged)
{
    repository.files["child.xml"] = bytes(R"xml(
      <rom><romid><xmlid>CHILD</xmlid></romid><include>BASE</include></rom>)xml");
    repository.read_errors["base.xml"] =
        Error{ErrorKind::InvalidConfig, "parent definition read failed"};
    auto catalog = DefinitionCatalog::create({
        load_entry(DefinitionFormat::EcuFlash, "CHILD", "child.xml"),
        load_entry(DefinitionFormat::EcuFlash, "BASE", "base.xml"),
    });
    ASSERT_TRUE(catalog);

    auto result = service.load(*catalog, DefinitionFormat::EcuFlash, "CHILD");

    ASSERT_FALSE(result);
    EXPECT_EQ(
        result.error(),
        (Error{ErrorKind::InvalidConfig, "parent definition read failed"}));
    EXPECT_EQ(repository.read_counts["child.xml"], 1);
    EXPECT_EQ(repository.read_counts["base.xml"], 1);
}

TEST_F(DefinitionServiceTest, LoadRejectsMissingCatalogIdWithoutReading)
{
    auto catalog = DefinitionCatalog::create(
        {load_entry(DefinitionFormat::EcuFlash, "KNOWN", "known.xml")});
    ASSERT_TRUE(catalog);

    auto result = service.load(*catalog, DefinitionFormat::EcuFlash, "UNKNOWN");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(repository.read_counts.empty());
}

TEST_F(DefinitionServiceTest, LoadRejectsEcuFlashIdentityThatDoesNotMatchCatalogId)
{
    auto catalog = DefinitionCatalog::create(
        {load_entry(DefinitionFormat::EcuFlash, "EXPECTED", "stale.xml")});
    ASSERT_TRUE(catalog);
    repository.files["stale.xml"] = ecuflash_xml("OTHER");

    auto result = service.load(*catalog, DefinitionFormat::EcuFlash, "EXPECTED");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("EXPECTED"), std::string::npos);
    EXPECT_NE(result.error().detail.find("OTHER"), std::string::npos);
    EXPECT_NE(result.error().detail.find("stale.xml"), std::string::npos);
}

TEST_F(DefinitionServiceTest, LoadPropagatesDefinitionParseFailure)
{
    auto catalog = DefinitionCatalog::create(
        {load_entry(DefinitionFormat::RomRaider, "BROKEN", "broken.xml")});
    ASSERT_TRUE(catalog);
    repository.files["broken.xml"] = bytes("<not-roms/>");

    auto result = service.load(*catalog, DefinitionFormat::RomRaider, "BROKEN");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_counts["broken.xml"], 1);
}

TEST_F(DefinitionServiceTest, LoadPropagatesResolutionFailure)
{
    auto catalog = DefinitionCatalog::create(
        {load_entry(DefinitionFormat::EcuFlash, "CHILD", "child.xml")});
    ASSERT_TRUE(catalog);
    repository.files["child.xml"] = bytes(R"xml(
      <rom><romid><xmlid>CHILD</xmlid></romid><include>MISSING</include></rom>)xml");

    auto result = service.load(*catalog, DefinitionFormat::EcuFlash, "CHILD");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("MISSING"), std::string::npos);
    EXPECT_EQ(repository.read_counts["child.xml"], 1);
}

} // namespace
} // namespace fastecu::definition
