#include "src/backend/definition/definition_model.h"

#include <gtest/gtest.h>

#include <string>

using fastecu::ErrorKind;
using fastecu::definition::AxisDefinition;
using fastecu::definition::CalibrationMap;
using fastecu::definition::DefinitionCatalog;
using fastecu::definition::DefinitionFormat;
using fastecu::definition::DefinitionIndexEntry;
using fastecu::definition::IdEncoding;
using fastecu::definition::RomDefinition;
using fastecu::definition::Scaling;

namespace
{

DefinitionIndexEntry entry(std::string definition_id, std::string source,
                           std::vector<std::string> parents = {})
{
    return {
        .format = DefinitionFormat::RomRaider,
        .definition_id = std::move(definition_id),
        .internal_id = "CAL-001",
        .internal_id_address = 0x1234,
        .internal_id_encoding = IdEncoding::Ascii,
        .ecu_id = "ECU-001",
        .source = std::move(source),
        .parents = std::move(parents),
    };
}

TEST(DefinitionModelTest, DefaultsPreserveLegacyValueBuiltSemantics)
{
    const Scaling scaling;
    EXPECT_FALSE(scaling.supplied.tracked);
    EXPECT_FALSE(scaling.supplied.from_byte);
    EXPECT_FALSE(scaling.supplied.to_byte);
    EXPECT_FALSE(scaling.supplied.format);

    const AxisDefinition axis;
    EXPECT_EQ(axis.size, 1U);
    EXPECT_EQ(axis.from_byte, "x");
    EXPECT_EQ(axis.to_byte, "x");
    EXPECT_EQ(axis.start_position, "1");
    EXPECT_EQ(axis.interval, "1");
    EXPECT_FALSE(axis.supplied.tracked);
    EXPECT_FALSE(axis.supplied.static_data);

    const CalibrationMap map;
    EXPECT_EQ(map.x_size, 1U);
    EXPECT_EQ(map.y_size, 1U);
    EXPECT_FALSE(map.swap_xy);
    EXPECT_FALSE(map.flip_x);
    EXPECT_FALSE(map.flip_y);
    EXPECT_EQ(map.start_position, "1");
    EXPECT_EQ(map.interval, "1");
    EXPECT_FALSE(map.supplied.tracked);
    EXPECT_FALSE(map.supplied.stable_id);

    const RomDefinition definition{};
    EXPECT_TRUE(definition.resolved_sources.empty());
    EXPECT_TRUE(definition.resolved_definition_ids.empty());
}

TEST(DefinitionModelTest, PresenceMetadataParticipatesInValueEquality)
{
    AxisDefinition original;
    AxisDefinition supplied = original;
    supplied.supplied.tracked = true;
    supplied.supplied.size = true;

    EXPECT_NE(original, supplied);
    EXPECT_EQ(original, AxisDefinition{});
}

TEST(DefinitionCatalogTest, FindsEntryByFormatAndDefinitionId)
{
    auto romraider = entry("ROMRAIDER", "romraider.xml");
    auto ecuflash = entry("ECUFLASH", "ecuflash.xml");
    ecuflash.format = DefinitionFormat::EcuFlash;

    auto catalog = DefinitionCatalog::create({romraider, ecuflash});

    ASSERT_TRUE(catalog);
    auto found = catalog->find(DefinitionFormat::EcuFlash, "ECUFLASH");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->get().source, "ecuflash.xml");
    EXPECT_EQ(catalog->entries().size(), 2u);
}

TEST(DefinitionCatalogTest, KeepsFirstOfIdenticalDuplicatesFromDifferentSources)
{
    auto first = entry("A", "first.xml", {"BASE"});
    auto second = first;
    second.source = "second.xml";

    auto catalog = DefinitionCatalog::create({first, second});

    ASSERT_TRUE(catalog);
    ASSERT_EQ(catalog->entries().size(), 1u);
    EXPECT_EQ(catalog->entries().front().source, "first.xml");
}

TEST(DefinitionCatalogTest, ConflictingDuplicateInternalIdIsInvalidConfig)
{
    auto first = entry("A", "a.xml");
    auto second = entry("A", "b.xml");
    second.internal_id = "CAL-002";

    auto result = DefinitionCatalog::create({first, second});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("a.xml"), std::string::npos);
    EXPECT_NE(result.error().detail.find("b.xml"), std::string::npos);
}

TEST(DefinitionCatalogTest, ConflictingDuplicateEcuIdentityIsInvalidConfig)
{
    auto first = entry("A", "a.xml");
    auto second = entry("A", "b.xml");
    second.ecu_id = "ECU-002";

    auto result = DefinitionCatalog::create({first, second});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find("a.xml"), std::string::npos);
    EXPECT_NE(result.error().detail.find("b.xml"), std::string::npos);
}

TEST(DefinitionCatalogTest, ConflictingDuplicateAddressIsInvalidConfig)
{
    auto first = entry("A", "a.xml");
    auto second = entry("A", "b.xml");
    second.internal_id_address = 0x5678;

    auto result = DefinitionCatalog::create({first, second});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(DefinitionCatalogTest, ConflictingDuplicateEncodingIsInvalidConfig)
{
    auto first = entry("A", "a.xml");
    auto second = entry("A", "b.xml");
    second.internal_id_encoding = IdEncoding::Hex;

    auto result = DefinitionCatalog::create({first, second});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(DefinitionCatalogTest, ConflictingDuplicateParentsIsInvalidConfig)
{
    auto first = entry("A", "a.xml", {"BASE"});
    auto second = entry("A", "b.xml", {"OTHER"});

    auto result = DefinitionCatalog::create({first, second});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(DefinitionCatalogTest, EmptyDefinitionIdIsInvalidConfig)
{
    auto result = DefinitionCatalog::create({entry("", "definition.xml")});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(DefinitionCatalogTest, MissingSourceIsInvalidConfig)
{
    auto result = DefinitionCatalog::create({entry("A", "")});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(DefinitionCatalogTest, EmptyParentReferenceIsInvalidConfig)
{
    auto result = DefinitionCatalog::create({entry("A", "definition.xml", {""})});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

TEST(DefinitionCatalogTest, MissingEntryIsInvalidConfig)
{
    auto catalog = DefinitionCatalog::create({entry("A", "definition.xml")});

    ASSERT_TRUE(catalog);
    auto result = catalog->find(DefinitionFormat::RomRaider, "UNKNOWN");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

} // namespace
