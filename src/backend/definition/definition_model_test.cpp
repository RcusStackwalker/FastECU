#include "src/backend/definition/definition_model.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

using fastecu::ErrorKind;
using fastecu::definition::AxisDefinition;
using fastecu::definition::CalibrationMap;
using fastecu::definition::DefinitionCatalog;
using fastecu::definition::DefinitionFormat;
using fastecu::definition::DefinitionIndexEntry;
using fastecu::definition::find_scaling;
using fastecu::definition::IdEncoding;
using fastecu::definition::is_unsigned_storage;
using fastecu::definition::RomDefinition;
using fastecu::definition::Scaling;
using fastecu::definition::StorageType;
using fastecu::definition::UnresolvedAxisDefinition;
using fastecu::definition::UnresolvedCalibrationMap;
using fastecu::definition::UnresolvedDefinition;
using fastecu::definition::UnresolvedScaling;

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

TEST(DefinitionModelTest, UnresolvedFieldsDistinguishOmittedFromExplicitDefaults)
{
    const UnresolvedScaling omitted_scaling;
    EXPECT_FALSE(omitted_scaling.from_byte);
    EXPECT_FALSE(omitted_scaling.to_byte);
    EXPECT_FALSE(omitted_scaling.format);

    UnresolvedAxisDefinition explicit_axis;
    explicit_axis.size = 1U;
    explicit_axis.from_byte = "x";
    explicit_axis.static_data = std::vector<std::string>{};
    ASSERT_TRUE(explicit_axis.size);
    EXPECT_EQ(*explicit_axis.size, 1U);
    ASSERT_TRUE(explicit_axis.from_byte);
    EXPECT_EQ(*explicit_axis.from_byte, "x");
    EXPECT_TRUE(explicit_axis.static_data);
    EXPECT_TRUE(explicit_axis.static_data->empty());

    UnresolvedCalibrationMap explicit_map;
    explicit_map.x_size = 1U;
    explicit_map.swap_xy = false;
    ASSERT_TRUE(explicit_map.x_size);
    EXPECT_EQ(*explicit_map.x_size, 1U);
    ASSERT_TRUE(explicit_map.swap_xy);
    EXPECT_FALSE(*explicit_map.swap_xy);
}

TEST(DefinitionModelTest, ResolvedValuesHaveConcreteDefaults)
{
    const Scaling scaling;
    EXPECT_EQ(scaling.from_byte, "x");
    EXPECT_EQ(scaling.to_byte, "x");

    const AxisDefinition axis;
    EXPECT_EQ(axis.size, 1U);
    EXPECT_EQ(axis.from_byte, "x");
    EXPECT_EQ(axis.to_byte, "x");
    EXPECT_EQ(axis.start_position, 1U);
    EXPECT_EQ(axis.interval, 1U);
    EXPECT_FALSE(axis.storage_type.has_value());

    const CalibrationMap map;
    EXPECT_EQ(map.x_size, 1U);
    EXPECT_EQ(map.y_size, 1U);
    EXPECT_FALSE(map.swap_xy);
    EXPECT_FALSE(map.flip_x);
    EXPECT_FALSE(map.flip_y);
    EXPECT_EQ(map.start_position, 1U);
    EXPECT_EQ(map.interval, 1U);
    EXPECT_FALSE(map.storage_type.has_value());

    const RomDefinition definition{};
    EXPECT_TRUE(definition.resolved_sources.empty());
    EXPECT_TRUE(definition.resolved_definition_ids.empty());
}

TEST(DefinitionModelTest, ResolvedDefinitionIsDistinctFromUnresolvedInput)
{
    static_assert(!std::is_base_of_v<UnresolvedDefinition, RomDefinition>);
    static_assert(
        std::is_same_v<decltype(UnresolvedDefinition::maps),
                       std::vector<UnresolvedCalibrationMap>>);
    static_assert(
        std::is_same_v<decltype(RomDefinition::maps),
                       std::vector<CalibrationMap>>);
}

TEST(StorageByteSizeTest, MapsEachStorageTypeToItsByteWidth)
{
    using fastecu::definition::storage_byte_size;
    using fastecu::definition::StorageType;

    EXPECT_EQ(storage_byte_size(StorageType::Uint8), 1U);
    EXPECT_EQ(storage_byte_size(StorageType::Int8), 1U);
    EXPECT_EQ(storage_byte_size(StorageType::Uint16), 2U);
    EXPECT_EQ(storage_byte_size(StorageType::Int16), 2U);
    EXPECT_EQ(storage_byte_size(StorageType::Uint24), 3U);
    EXPECT_EQ(storage_byte_size(StorageType::Int24), 3U);
    EXPECT_EQ(storage_byte_size(StorageType::Uint32), 4U);
    EXPECT_EQ(storage_byte_size(StorageType::Int32), 4U);
    EXPECT_EQ(storage_byte_size(StorageType::Float), 4U);
    EXPECT_EQ(storage_byte_size(StorageType::Bloblist), 1U);
}

TEST(StorageByteSizeTest, DefaultsToOneByteWhenStorageTypeIsAbsent)
{
    using fastecu::definition::storage_byte_size;
    using fastecu::definition::StorageType;

    EXPECT_EQ(storage_byte_size(std::optional<StorageType>{}), 1U);
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

TEST(DefinitionModel, FindScalingReturnsMatchingEntry)
{
    RomDefinition definition;
    definition.scalings.push_back(Scaling{.name = "Fuel"});
    definition.scalings.push_back(Scaling{.name = "Timing"});

    const Scaling *found = find_scaling(definition, "Timing");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "Timing");
}

TEST(DefinitionModel, FindScalingReturnsNullWhenAbsent)
{
    RomDefinition definition;
    definition.scalings.push_back(Scaling{.name = "Fuel"});

    EXPECT_EQ(find_scaling(definition, "Missing"), nullptr);
    EXPECT_EQ(find_scaling(definition, ""), nullptr);
    EXPECT_EQ(find_scaling(RomDefinition{}, "Fuel"), nullptr);
}

TEST(DefinitionModel, IsUnsignedStorageCoversEveryStorageType)
{
    EXPECT_TRUE(is_unsigned_storage(StorageType::Uint8));
    EXPECT_TRUE(is_unsigned_storage(StorageType::Uint16));
    EXPECT_TRUE(is_unsigned_storage(StorageType::Uint24));
    EXPECT_TRUE(is_unsigned_storage(StorageType::Uint32));
    EXPECT_FALSE(is_unsigned_storage(StorageType::Int8));
    EXPECT_FALSE(is_unsigned_storage(StorageType::Int16));
    EXPECT_FALSE(is_unsigned_storage(StorageType::Int24));
    EXPECT_FALSE(is_unsigned_storage(StorageType::Int32));
    EXPECT_FALSE(is_unsigned_storage(StorageType::Float));
    EXPECT_FALSE(is_unsigned_storage(StorageType::Bloblist));
    // Absent storage type: legacy tested `storagetype.startsWith("uint")` on
    // an empty QString, which is false. Matched exactly.
    EXPECT_FALSE(is_unsigned_storage(std::nullopt));
}

} // namespace
