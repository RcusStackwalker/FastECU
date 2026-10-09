#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_file_system.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
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
    return bytes("<roms><rom><romid><xmlid>" + std::string(id) + "</xmlid></romid></rom></roms>");
}

std::vector<std::uint8_t> ecuflash_xml(std::string_view id)
{
    return bytes("<rom><romid><xmlid>" + std::string(id) + "</xmlid></romid></rom>");
}

DefinitionIndexEntry index_entry(std::string id, std::string internal_id, std::optional<std::uint64_t> address,
                                 IdEncoding encoding = IdEncoding::kAscii)
{
    return DefinitionIndexEntry{
        .format = DefinitionFormat::kRomRaider,
        .definition_id = std::move(id),
        .internal_id = std::move(internal_id),
        .internal_id_address = address,
        .internal_id_encoding = encoding,
        .source = "definitions.xml",
    };
}

DefinitionIndexEntry load_entry(DefinitionFormat format, std::string id, std::string source)
{
    DefinitionIndexEntry entry = index_entry(std::move(id), "", std::nullopt);
    entry.format = format;
    entry.source = std::move(source);
    return entry;
}

class DefinitionServiceTest : public ::testing::Test
{
  protected:
    InMemoryFileSystem file_system_;
    InMemoryFileRepository repository_;
    InMemoryAtomicFileWriter writer_;
    DefinitionService service_{file_system_, repository_, writer_};
};

DefinitionHeaderInput valid_header_input()
{
    return DefinitionHeaderInput{
        .xml_id = "NEW",
        .internal_id = "INTERNAL",
        .ecu_id = "ECU",
        .internal_id_address = 0x20,
        .metadata =
            RomMetadata{
                .make = "Subaru",
                .market = "US",
                .model = "Legacy",
                .year = "2009",
            },
        .include = "BASE",
        .notes = "Document notes",
    };
}

TEST_F(DefinitionServiceTest, DiscoversEcuFlashXmlRecursivelyInLexicalFullPathOrder)
{
    file_system_.directory_entries["defs"] = {
        DirEntry{.name = "z.xml", .is_directory = false},
        DirEntry{.name = "nested", .is_directory = true},
        DirEntry{.name = "notes.txt", .is_directory = false},
        DirEntry{.name = "pretend.xml", .is_directory = true},
    };
    file_system_.directory_entries["defs/nested"] = {
        DirEntry{.name = "b.XML", .is_directory = false},
        DirEntry{.name = "a.Xml", .is_directory = false},
    };
    file_system_.directory_entries["defs/pretend.xml"] = {
        DirEntry{.name = "ignored.bin", .is_directory = false},
    };
    repository_.files["defs/z.xml"] = ecuflash_xml("Z");
    repository_.files["defs/nested/b.XML"] = ecuflash_xml("B");
    repository_.files["defs/nested/a.Xml"] = ecuflash_xml("A");

    auto result = service_.build_ecuflash_catalog("defs");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->entries().size(), 3U);
    EXPECT_EQ(result->entries()[0].definition_id, "A");
    EXPECT_EQ(result->entries()[1].definition_id, "B");
    EXPECT_EQ(result->entries()[2].definition_id, "Z");
    EXPECT_EQ(repository_.read_count("defs/nested/a.Xml"), 1);
    EXPECT_EQ(repository_.read_count("defs/nested/b.XML"), 1);
    EXPECT_EQ(repository_.read_count("defs/z.xml"), 1);
    EXPECT_FALSE(repository_.read_count("defs/notes.txt") != 0);
    EXPECT_FALSE(repository_.read_count("defs/pretend.xml") != 0);
}

TEST_F(DefinitionServiceTest, SkipsSymlinkDirectoriesWhileDiscoveringOrdinaryNestedXml)
{
    file_system_.directory_entries["defs"] = {
        DirEntry{.name = "nested", .is_directory = true},
    };
    file_system_.directory_entries["defs/nested"] = {
        DirEntry{.name = "definition.xml", .is_directory = false},
        DirEntry{
            .name = "loop",
            .is_directory = true,
            .is_symlink = true,
        },
    };
    repository_.files["defs/nested/definition.xml"] = ecuflash_xml("NESTED");

    auto result = service_.build_ecuflash_catalog("defs");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->entries().size(), 1U);
    EXPECT_EQ(result->entries()[0].definition_id, "NESTED");
    EXPECT_EQ(result->entries()[0].source, "defs/nested/definition.xml");
    EXPECT_FALSE(repository_.read_count("defs/nested/loop") != 0);
}

TEST_F(DefinitionServiceTest, MergesExplicitEcuFlashHandlesDeterministicallyAndReadsEachSourceOnce)
{
    file_system_.directory_entries["defs"] = {
        DirEntry{.name = "z.xml", .is_directory = false},
    };
    repository_.files["defs/z.xml"] = ecuflash_xml("Z");
    repository_.files["external/a.xml"] = ecuflash_xml("A");
    repository_.files["external/b.xml"] = ecuflash_xml("B");
    const std::vector<std::string> explicit_handles{
        "external/b.xml",
        "defs/z.xml",
        "external/a.xml",
        "external/b.xml",
    };

    auto result = service_.build_ecuflash_catalog("defs", explicit_handles);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->entries().size(), 3U);
    EXPECT_EQ(result->entries()[0].definition_id, "Z");
    EXPECT_EQ(result->entries()[0].source, "defs/z.xml");
    EXPECT_EQ(result->entries()[1].definition_id, "A");
    EXPECT_EQ(result->entries()[1].source, "external/a.xml");
    EXPECT_EQ(result->entries()[2].definition_id, "B");
    EXPECT_EQ(result->entries()[2].source, "external/b.xml");
    EXPECT_EQ(repository_.read_count("defs/z.xml"), 1);
    EXPECT_EQ(repository_.read_count("external/a.xml"), 1);
    EXPECT_EQ(repository_.read_count("external/b.xml"), 1);
}

TEST_F(DefinitionServiceTest, BuildsEcuFlashCatalogFromExplicitHandlesWithoutConfiguredDirectory)
{
    repository_.files["external/definition.xml"] = bytes(R"xml(
      <rom><romid><xmlid>XML_ID</xmlid><internalidaddress>0</internalidaddress>
      <internalidstring>INTERNAL_ID</internalidstring></romid></rom>)xml");
    const std::vector<std::string> explicit_handles{
        "external/definition.xml",
    };

    auto result = service_.build_ecuflash_catalog({}, explicit_handles);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->entries().size(), 1U);
    EXPECT_EQ(result->entries()[0].definition_id, "XML_ID");
    EXPECT_EQ(result->entries()[0].internal_id, "INTERNAL_ID");
    EXPECT_EQ(result->entries()[0].source, "external/definition.xml");
}

TEST_F(DefinitionServiceTest, PreservesConfiguredRomRaiderHandleOrderingAndReadsEachOnce)
{
    repository_.files["second.xml"] = romraider_xml("SECOND");
    repository_.files["first.xml"] = romraider_xml("FIRST");
    const std::vector<std::string> handles{"second.xml", "first.xml"};

    auto result = service_.build_romraider_catalog(handles);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->entries().size(), 2U);
    EXPECT_EQ(result->entries()[0].definition_id, "SECOND");
    EXPECT_EQ(result->entries()[1].definition_id, "FIRST");
    EXPECT_EQ(repository_.read_count("second.xml"), 1);
    EXPECT_EQ(repository_.read_count("first.xml"), 1);
}

TEST_F(DefinitionServiceTest, PropagatesDiscoveryFailureWithoutReadingFiles)
{
    file_system_.list_directory_errors["defs"] = Error{ErrorKind::kDisconnected, "catalog directory unavailable"};

    auto result = service_.build_ecuflash_catalog("defs");

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(result.error(), (Error{ErrorKind::kDisconnected, "catalog directory unavailable"}));
    EXPECT_TRUE(repository_.read_handles.empty());
}

TEST_F(DefinitionServiceTest, SkipsHandleThatFailsToReadAndKeepsRemainingEntries)
{
    const std::vector<std::string> handles{"bad.xml", "good.xml"};
    repository_.read_errors["bad.xml"] = Error{ErrorKind::kDisconnected, "read failed"};
    repository_.files["good.xml"] = romraider_xml("GOOD");

    auto result = service_.build_romraider_catalog(handles);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->entries().size(), 1U);
    EXPECT_EQ(result->entries()[0].definition_id, "GOOD");
    EXPECT_EQ(repository_.read_count("bad.xml"), 1);
    EXPECT_EQ(repository_.read_count("good.xml"), 1);
}

TEST_F(DefinitionServiceTest, SkipsUnparsableFileAfterOneReadAndKeepsRemainingEntries)
{
    file_system_.directory_entries["defs"] = {
        DirEntry{.name = "bad.xml", .is_directory = false},
        DirEntry{.name = "good.xml", .is_directory = false},
    };
    repository_.files["defs/bad.xml"] = bytes("<not-rom/>");
    repository_.files["defs/good.xml"] = ecuflash_xml("GOOD");

    auto result = service_.build_ecuflash_catalog("defs");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->entries().size(), 1U);
    EXPECT_EQ(result->entries()[0].definition_id, "GOOD");
    EXPECT_EQ(repository_.read_count("defs/bad.xml"), 1);
    EXPECT_EQ(repository_.read_count("defs/good.xml"), 1);
}

TEST_F(DefinitionServiceTest, SkipsEveryUnusableHandleAndSucceedsWithAnEmptyCatalog)
{
    const std::vector<std::string> handles{"bad.xml"};
    repository_.read_errors["bad.xml"] = Error{ErrorKind::kDisconnected, "read failed"};

    auto result = service_.build_romraider_catalog(handles);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(result->entries().empty());
}

TEST_F(DefinitionServiceTest, MatchesAsciiIdentifierEndingExactlyAtRomEnd)
{
    auto catalog = DefinitionCatalog::create({index_entry("EXACT", "AB", 1U)});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{'x', 'A', 'B'};

    auto result = service_.match_rom(*catalog, rom);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->definition_id, "EXACT");
}

TEST_F(DefinitionServiceTest, RejectsIdentifierWhenRomIsOneByteShort)
{
    auto catalog = DefinitionCatalog::create({index_entry("SHORT", "AB", 1U)});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{'x', 'A'};

    ASSERT_THAT(service_.match_rom(*catalog, rom), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST_F(DefinitionServiceTest, MatchesUpperAndLowerCaseHexText)
{
    auto lower_catalog = DefinitionCatalog::create({index_entry("LOWER", "ab10", 0U, IdEncoding::kHex)});
    auto upper_catalog = DefinitionCatalog::create({index_entry("UPPER", "AB10", 0U, IdEncoding::kHex)});
    ASSERT_THAT(lower_catalog, fastecu::testing::IsOk());
    ASSERT_THAT(upper_catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{0xAB, 0x10};

    auto lower = service_.match_rom(*lower_catalog, rom);
    auto upper = service_.match_rom(*upper_catalog, rom);

    ASSERT_THAT(lower, fastecu::testing::IsOk());
    ASSERT_THAT(upper, fastecu::testing::IsOk());
    EXPECT_EQ(lower->definition_id, "LOWER");
    EXPECT_EQ(upper->definition_id, "UPPER");
}

TEST_F(DefinitionServiceTest, MatchesEitherAsciiOrHexForLegacyCompatibleEntry)
{
    auto ascii_catalog = DefinitionCatalog::create({index_entry("ASCII", "AB10", 0U, IdEncoding::kAsciiOrHex)});
    auto hex_catalog = DefinitionCatalog::create({index_entry("HEX", "AB10", 0U, IdEncoding::kAsciiOrHex)});
    ASSERT_THAT(ascii_catalog, fastecu::testing::IsOk());
    ASSERT_THAT(hex_catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> ascii_rom{'A', 'B', '1', '0'};
    const std::vector<std::uint8_t> hex_rom{0xAB, 0x10};

    auto ascii = service_.match_rom(*ascii_catalog, ascii_rom);
    auto hex = service_.match_rom(*hex_catalog, hex_rom);

    ASSERT_THAT(ascii, fastecu::testing::IsOk());
    ASSERT_THAT(hex, fastecu::testing::IsOk());
    EXPECT_EQ(ascii->definition_id, "ASCII");
    EXPECT_EQ(hex->definition_id, "HEX");
}

TEST_F(DefinitionServiceTest, SkipsOddLengthHexIdentifierAndMatchesLaterEntry)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("ODD", "ABC", 0U, IdEncoding::kHex),
        index_entry("VALID", "AB", 0U, IdEncoding::kHex),
    });
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{0xAB};

    auto result = service_.match_rom(*catalog, rom);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->definition_id, "VALID");
}

TEST_F(DefinitionServiceTest, SkipsInvalidHexDigitAndMatchesLaterEntry)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("INVALID_HEX", "AG", 0U, IdEncoding::kHex),
        index_entry("VALID", "41", 0U, IdEncoding::kHex),
    });
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service_.match_rom(*catalog, rom);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->definition_id, "VALID");
}

TEST_F(DefinitionServiceTest, SkipsMissingAddressAndMatchesLaterEntry)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("NO_ADDRESS", "A", std::nullopt),
        index_entry("VALID", "A", 0U),
    });
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service_.match_rom(*catalog, rom);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->definition_id, "VALID");
}

TEST_F(DefinitionServiceTest, SkipsAddressBeyondRomBoundsAndMatchesLaterEntry)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("OUT_OF_RANGE", "A", 2U),
        index_entry("VALID", "A", 0U),
    });
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service_.match_rom(*catalog, rom);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->definition_id, "VALID");
}

TEST_F(DefinitionServiceTest, ReturnsFirstCatalogEntryWhenMatchesAreAmbiguous)
{
    auto catalog = DefinitionCatalog::create({
        index_entry("FIRST", "A", 0U),
        index_entry("SECOND", "A", 0U),
    });
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{'A'};

    auto result = service_.match_rom(*catalog, rom);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->definition_id, "FIRST");
}

TEST_F(DefinitionServiceTest, EmptyIdentifierDoesNotMatch)
{
    auto catalog = DefinitionCatalog::create({index_entry("EMPTY", "", 0U)});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());

    ASSERT_THAT(service_.match_rom(*catalog, {}), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST_F(DefinitionServiceTest, ReturnsInvalidConfigWhenNoIdentifierMatches)
{
    auto catalog = DefinitionCatalog::create({index_entry("OTHER", "B", 0U)});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    const std::vector<std::uint8_t> rom{'A'};

    ASSERT_THAT(service_.match_rom(*catalog, rom), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST_F(DefinitionServiceTest, LoadsAndResolvesRomRaiderChildAndBaseFiles)
{
    repository_.files["child.xml"] = bytes(R"xml(
      <roms><rom base="BASE"><romid><xmlid>CHILD</xmlid></romid></rom></roms>)xml");
    repository_.files["base.xml"] = romraider_xml("BASE");
    auto catalog = DefinitionCatalog::create({
        load_entry(DefinitionFormat::kRomRaider, "CHILD", "child.xml"),
        load_entry(DefinitionFormat::kRomRaider, "BASE", "base.xml"),
    });
    ASSERT_THAT(catalog, fastecu::testing::IsOk());

    auto result = service_.load(*catalog, DefinitionFormat::kRomRaider, "CHILD");

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->identity.xml_id, "CHILD");
    EXPECT_EQ(result->resolved_sources, (std::vector<std::string>{"base.xml", "child.xml"}));
    EXPECT_EQ(repository_.read_count("child.xml"), 1);
    EXPECT_EQ(repository_.read_count("base.xml"), 1);
}

TEST_F(DefinitionServiceTest, MemoizesRepeatedEcuFlashParentOncePerLoadCall)
{
    repository_.files["root.xml"] = bytes(R"xml(
      <rom><romid><xmlid>ROOT</xmlid></romid>
      <include>LEFT</include><include>RIGHT</include></rom>)xml");
    repository_.files["left.xml"] = bytes(R"xml(
      <rom><romid><xmlid>LEFT</xmlid></romid><include>BASE</include></rom>)xml");
    repository_.files["right.xml"] = bytes(R"xml(
      <rom><romid><xmlid>RIGHT</xmlid></romid><include>BASE</include></rom>)xml");
    repository_.files["base.xml"] = ecuflash_xml("BASE");
    auto catalog = DefinitionCatalog::create({
        load_entry(DefinitionFormat::kEcuFlash, "ROOT", "root.xml"),
        load_entry(DefinitionFormat::kEcuFlash, "LEFT", "left.xml"),
        load_entry(DefinitionFormat::kEcuFlash, "RIGHT", "right.xml"),
        load_entry(DefinitionFormat::kEcuFlash, "BASE", "base.xml"),
    });
    ASSERT_THAT(catalog, fastecu::testing::IsOk());

    auto first = service_.load(*catalog, DefinitionFormat::kEcuFlash, "ROOT");
    auto second = service_.load(*catalog, DefinitionFormat::kEcuFlash, "ROOT");

    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_THAT(second, fastecu::testing::IsOk());
    EXPECT_EQ(first->identity.xml_id, "ROOT");
    EXPECT_EQ(second->identity.xml_id, "ROOT");
    EXPECT_EQ(repository_.read_count("root.xml"), 2);
    EXPECT_EQ(repository_.read_count("left.xml"), 2);
    EXPECT_EQ(repository_.read_count("right.xml"), 2);
    EXPECT_EQ(repository_.read_count("base.xml"), 2);
}

TEST_F(DefinitionServiceTest, LoadPropagatesRepositoryFailure)
{
    auto catalog = DefinitionCatalog::create({load_entry(DefinitionFormat::kEcuFlash, "BROKEN", "broken.xml")});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    repository_.read_errors["broken.xml"] = Error{ErrorKind::kDisconnected, "definition read failed"};

    auto result = service_.load(*catalog, DefinitionFormat::kEcuFlash, "BROKEN");

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(result.error(), (Error{ErrorKind::kDisconnected, "definition read failed"}));
}

TEST_F(DefinitionServiceTest, LoadPropagatesParentRepositoryFailureUnchanged)
{
    repository_.files["child.xml"] = bytes(R"xml(
      <rom><romid><xmlid>CHILD</xmlid></romid><include>BASE</include></rom>)xml");
    repository_.read_errors["base.xml"] = Error{ErrorKind::kInvalidConfig, "parent definition read failed"};
    auto catalog = DefinitionCatalog::create({
        load_entry(DefinitionFormat::kEcuFlash, "CHILD", "child.xml"),
        load_entry(DefinitionFormat::kEcuFlash, "BASE", "base.xml"),
    });
    ASSERT_THAT(catalog, fastecu::testing::IsOk());

    auto result = service_.load(*catalog, DefinitionFormat::kEcuFlash, "CHILD");

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(result.error(), (Error{ErrorKind::kInvalidConfig, "parent definition read failed"}));
    EXPECT_EQ(repository_.read_count("child.xml"), 1);
    EXPECT_EQ(repository_.read_count("base.xml"), 1);
}

TEST_F(DefinitionServiceTest, LoadRejectsMissingCatalogIdWithoutReading)
{
    auto catalog = DefinitionCatalog::create({load_entry(DefinitionFormat::kEcuFlash, "KNOWN", "known.xml")});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());

    ASSERT_THAT(service_.load(*catalog, DefinitionFormat::kEcuFlash, "UNKNOWN"),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(repository_.read_handles.empty());
}

TEST_F(DefinitionServiceTest, LoadRejectsEcuFlashIdentityThatDoesNotMatchCatalogId)
{
    auto catalog = DefinitionCatalog::create({load_entry(DefinitionFormat::kEcuFlash, "EXPECTED", "stale.xml")});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    repository_.files["stale.xml"] = ecuflash_xml("OTHER");

    auto result = service_.load(*catalog, DefinitionFormat::kEcuFlash, "EXPECTED");

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("EXPECTED"));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("OTHER"));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("stale.xml"));
}

TEST_F(DefinitionServiceTest, LoadPropagatesDefinitionParseFailure)
{
    auto catalog = DefinitionCatalog::create({load_entry(DefinitionFormat::kRomRaider, "BROKEN", "broken.xml")});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    repository_.files["broken.xml"] = bytes("<not-roms/>");

    ASSERT_THAT(service_.load(*catalog, DefinitionFormat::kRomRaider, "BROKEN"),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(repository_.read_count("broken.xml"), 1);
}

TEST_F(DefinitionServiceTest, LoadPropagatesResolutionFailure)
{
    auto catalog = DefinitionCatalog::create({load_entry(DefinitionFormat::kEcuFlash, "CHILD", "child.xml")});
    ASSERT_THAT(catalog, fastecu::testing::IsOk());
    repository_.files["child.xml"] = bytes(R"xml(
      <rom><romid><xmlid>CHILD</xmlid></romid><include>MISSING</include></rom>)xml");

    auto result = service_.load(*catalog, DefinitionFormat::kEcuFlash, "CHILD");

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("MISSING"));
    EXPECT_EQ(repository_.read_count("child.xml"), 1);
}

TEST_F(DefinitionServiceTest, CreatesDefinitionWithOneExactAtomicReplacement)
{
    const DefinitionHeaderInput input = valid_header_input();
    auto expected = create_ecuflash_xml(input);
    ASSERT_THAT(expected, fastecu::testing::IsOk());

    ASSERT_THAT(service_.create_definition("created.xml", input), fastecu::testing::IsOk());
    ASSERT_EQ(writer_.replace_calls.size(), 1U);
    EXPECT_EQ(writer_.replace_calls.front().handle, "created.xml");
    EXPECT_EQ(writer_.replace_calls.front().data, *expected);
}

TEST_F(DefinitionServiceTest, RejectsCreationWhenDestinationAlreadyExists)
{
    const DefinitionHeaderInput input = valid_header_input();
    file_system_.files["existing.xml"] = bytes("<rom><romid><xmlid>EXISTING</xmlid></romid></rom>");

    auto result = service_.create_definition("existing.xml", input);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("existing.xml"));
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DefinitionServiceTest, RejectsInvalidCreationInputBeforeAtomicReplacement)
{
    DefinitionHeaderInput input = valid_header_input();
    input.internal_id = " \t ";

    ASSERT_THAT(service_.create_definition("untouched.xml", input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DefinitionServiceTest, ImportsDefinitionWithOneExactAtomicReplacement)
{
    const DefinitionHeaderInput input = valid_header_input();
    repository_.files["source.xml"] = bytes(R"xml(
<rom>
  <!-- preserved -->
  <romid><xmlid>OLD</xmlid></romid>
  <include>OLD_BASE</include>
  <vendor-extension/>
</rom>)xml");
    auto expected = rewrite_ecuflash_xml(repository_.files["source.xml"], input);
    ASSERT_THAT(expected, fastecu::testing::IsOk());

    ASSERT_THAT(service_.import_definition("source.xml", "imported.xml", input), fastecu::testing::IsOk());
    EXPECT_EQ(repository_.read_count("source.xml"), 1);
    ASSERT_EQ(writer_.replace_calls.size(), 1U);
    EXPECT_EQ(writer_.replace_calls.front().handle, "imported.xml");
    EXPECT_EQ(writer_.replace_calls.front().data, *expected);
}

TEST_F(DefinitionServiceTest, ImportPropagatesSourceReadFailureBeforeAtomicReplacement)
{
    repository_.read_errors["source.xml"] = Error{ErrorKind::kDisconnected, "source vanished"};

    auto result = service_.import_definition("source.xml", "untouched.xml", valid_header_input());

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(result.error(), (Error{ErrorKind::kDisconnected, "source vanished"}));
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DefinitionServiceTest, ImportRejectsMalformedSourceBeforeAtomicReplacement)
{
    repository_.files["source.xml"] = bytes("<rom><romid>");

    ASSERT_THAT(service_.import_definition("source.xml", "untouched.xml", valid_header_input()),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DefinitionServiceTest, ImportRejectsInvalidTransformedTreeBeforeAtomicReplacement)
{
    repository_.files["source.xml"] = bytes(R"xml(
<rom>
  <romid><xmlid>OLD</xmlid></romid>
  <table name="Fuel" address="100">
    <table type="X Axis" elements="1"/>
  </table>
</rom>)xml");

    ASSERT_THAT(service_.import_definition("source.xml", "untouched.xml", valid_header_input()),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DefinitionServiceTest, ImportRejectsDuplicateRomIdBeforeAtomicReplacement)
{
    repository_.files["source.xml"] = bytes(R"xml(
<rom>
  <romid><xmlid>FIRST</xmlid></romid>
  <romid><xmlid>SECOND</xmlid></romid>
</rom>)xml");

    auto result = service_.import_definition("source.xml", "untouched.xml", valid_header_input());

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("<romid>"));
    EXPECT_TRUE(writer_.replace_calls.empty());
}

TEST_F(DefinitionServiceTest, PropagatesAtomicReplacementFailureUnchanged)
{
    writer_.replace_error = Error{ErrorKind::kInternal, "atomic commit failed"};

    auto result = service_.create_definition("destination.xml", valid_header_input());

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(result.error(), (Error{ErrorKind::kInternal, "atomic commit failed"}));
    ASSERT_EQ(writer_.replace_calls.size(), 1U);
    EXPECT_EQ(writer_.replace_calls.front().handle, "destination.xml");
}

} // namespace
} // namespace fastecu::definition
