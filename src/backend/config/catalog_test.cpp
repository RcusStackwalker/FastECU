#include "src/backend/config/catalog.h"

#include <array>
#include <cstddef>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::config::Catalog;
using fastecu::config::catalog_problems;
using fastecu::config::catalog_references_resolve;
using fastecu::config::checksum_flag;
using fastecu::config::ChecksumSupport;
using fastecu::config::kernel_load_address_text;
using fastecu::config::protocol_in;
using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

constexpr auto kProtocols = std::to_array<ProtocolSpec>({
    {.name = "proto_a",
     .alias = "alias_a",
     .mcu = "SH7058",
     .checksum = ChecksumSupport::kCorrected,
     .read = true,
     .kernel = "a.bin",
     .kernel_load_address = 0xFFFF3000U},
    {.name = "proto_b", .alias = "shared", .mcu = "M32R"},
    {.name = "proto_c", .alias = "shared", .mcu = "SH7055"},
});

// Rows 0 and 2 share proto_a. "shared" is the alias of proto_b and proto_c;
// proto_c's vehicle (row 1) comes before proto_b's (row 3).
constexpr auto kVehicles = std::to_array<VehicleSpec>({
    {.id = "subaru-impreza", .make = "Subaru", .model = "Impreza", .protocol = protocol_in(kProtocols, "proto_a")},
    {.id = "mitsubishi-colt", .make = "Mitsubishi", .model = "Colt", .protocol = protocol_in(kProtocols, "proto_c")},
    {.id = "subaru-forester", .make = "Subaru", .model = "Forester", .protocol = protocol_in(kProtocols, "proto_a")},
    {.id = "subaru-legacy", .make = "Subaru", .model = "Legacy", .protocol = protocol_in(kProtocols, "proto_b")},
});

constexpr Catalog kCatalog{kProtocols, kVehicles};

static_assert(protocol_in(kProtocols, "proto_b") == &kProtocols[1]);
static_assert(protocol_in(kProtocols, "absent") == nullptr);
static_assert(catalog_references_resolve(kProtocols, kVehicles));

TEST(ChecksumFlag, SpellsTheLegacyFlagText)
{
    EXPECT_EQ(checksum_flag(ChecksumSupport::kCorrected), "yes");
    EXPECT_EQ(checksum_flag(ChecksumSupport::kMissing), "n/a");
    EXPECT_EQ(checksum_flag(ChecksumSupport::kNone), "no");
}

TEST(KernelLoadAddressText, IsUnpaddedUppercaseHexOrEmpty)
{
    EXPECT_EQ(kernel_load_address_text(kProtocols[0]), "0xFFFF3000");
    EXPECT_EQ(kernel_load_address_text(ProtocolSpec{.kernel_load_address = 0x20000U}), "0x20000");
    EXPECT_EQ(kernel_load_address_text(kProtocols[1]), "");
}

TEST(Catalog, FindsAProtocolByName)
{
    EXPECT_EQ(kCatalog.find_protocol("proto_c"), &kProtocols[2]);
    EXPECT_EQ(kCatalog.find_protocol("absent"), nullptr);
}

TEST(Catalog, FindsAVehicleRowById)
{
    EXPECT_EQ(kCatalog.find_vehicle("subaru-forester"), std::optional<std::size_t>(2));
    EXPECT_EQ(kCatalog.find_vehicle("absent"), std::nullopt);
    EXPECT_EQ(kCatalog.find_vehicle(""), std::nullopt);
}

TEST(Catalog, ProtocolLookupTakesTheLastMatchingRow)
{
    EXPECT_EQ(kCatalog.last_vehicle_for_protocol("proto_a"), std::optional<std::size_t>(2));
    EXPECT_EQ(kCatalog.last_vehicle_for_protocol("proto_b"), std::optional<std::size_t>(3));
    EXPECT_EQ(kCatalog.last_vehicle_for_protocol("absent"), std::nullopt);
}

TEST(Catalog, AliasLookupTakesTheFirstMatchingVehicle)
{
    EXPECT_EQ(kCatalog.first_vehicle_for_alias("shared"), &kVehicles[1]);
    EXPECT_EQ(kCatalog.first_vehicle_for_alias("alias_a"), &kVehicles[0]);
    EXPECT_EQ(kCatalog.first_vehicle_for_alias("absent"), nullptr);
}

TEST(Catalog, AnEmptyFlashMethodMatchesNoAlias)
{
    static constexpr auto kUnaliased = std::to_array<ProtocolSpec>({{.name = "plain"}});
    static constexpr auto kOne =
        std::to_array<VehicleSpec>({{.id = "one", .protocol = protocol_in(kUnaliased, "plain")}});
    EXPECT_EQ((Catalog{kUnaliased, kOne}.first_vehicle_for_alias("")), nullptr);
}

TEST(CatalogProblems, AConsistentCatalogHasNone)
{
    EXPECT_THAT(catalog_problems(kCatalog), IsEmpty());
}

constexpr auto kBrokenProtocols = std::to_array<ProtocolSpec>({
    {.name = "dup"},
    {.name = "dup"},
    {.name = ""},
    {.name = "lonely", .alias = "a,b", .kernel_load_address = 0x1000U},
});
constexpr auto kForeignProtocols = std::to_array<ProtocolSpec>({{.name = "foreign"}});
constexpr auto kBrokenVehicles = std::to_array<VehicleSpec>({
    {.id = "Upper", .protocol = &kBrokenProtocols[0]},
    {.id = "twin", .protocol = &kBrokenProtocols[1]},
    {.id = "twin", .protocol = &kBrokenProtocols[2]},
    {.id = "orphan"},
    {.id = "stranger", .protocol = &kForeignProtocols[0]},
});

TEST(CatalogProblems, ReportsEachInconsistencyOnce)
{
    EXPECT_FALSE(catalog_references_resolve(kBrokenProtocols, kBrokenVehicles));
    EXPECT_THAT(catalog_problems(Catalog{kBrokenProtocols, kBrokenVehicles}),
                UnorderedElementsAre(
                    "duplicate protocol name 'dup'", "protocol name is empty",
                    "protocol 'lonely' alias 'a,b' contains ','",
                    "protocol 'lonely' has a kernel load address but no kernel", "protocol 'lonely' has no vehicle",
                    "vehicle id 'Upper' is not lowercase [a-z0-9-]", "duplicate vehicle id 'twin'",
                    "vehicle 'orphan' has no protocol", "vehicle 'stranger' protocol is not in this catalog"));
}

} // namespace
