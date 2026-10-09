#include "src/backend/config/catalog.h"

#include <array>
#include <cstddef>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::config::Catalog;
using fastecu::config::CatalogProblems;
using fastecu::config::CatalogReferencesResolve;
using fastecu::config::ChecksumFlag;
using fastecu::config::ChecksumSupport;
using fastecu::config::KernelLoadAddressText;
using fastecu::config::ProtocolIn;
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
    {.id = "subaru-impreza", .make = "Subaru", .model = "Impreza", .protocol = ProtocolIn(kProtocols, "proto_a")},
    {.id = "mitsubishi-colt", .make = "Mitsubishi", .model = "Colt", .protocol = ProtocolIn(kProtocols, "proto_c")},
    {.id = "subaru-forester", .make = "Subaru", .model = "Forester", .protocol = ProtocolIn(kProtocols, "proto_a")},
    {.id = "subaru-legacy", .make = "Subaru", .model = "Legacy", .protocol = ProtocolIn(kProtocols, "proto_b")},
});

constexpr Catalog kCatalog{kProtocols, kVehicles};

static_assert(ProtocolIn(kProtocols, "proto_b") == &kProtocols[1]);
static_assert(ProtocolIn(kProtocols, "absent") == nullptr);
static_assert(CatalogReferencesResolve(kProtocols, kVehicles));

TEST(ChecksumFlag, SpellsTheLegacyFlagText)
{
    EXPECT_EQ(ChecksumFlag(ChecksumSupport::kCorrected), "yes");
    EXPECT_EQ(ChecksumFlag(ChecksumSupport::kMissing), "n/a");
    EXPECT_EQ(ChecksumFlag(ChecksumSupport::kNone), "no");
}

TEST(KernelLoadAddressText, IsUnpaddedUppercaseHexOrEmpty)
{
    EXPECT_EQ(KernelLoadAddressText(kProtocols[0]), "0xFFFF3000");
    EXPECT_EQ(KernelLoadAddressText(ProtocolSpec{.kernel_load_address = 0x20000U}), "0x20000");
    EXPECT_EQ(KernelLoadAddressText(kProtocols[1]), "");
}

TEST(Catalog, FindsAProtocolByName)
{
    EXPECT_EQ(kCatalog.FindProtocol("proto_c"), &kProtocols[2]);
    EXPECT_EQ(kCatalog.FindProtocol("absent"), nullptr);
}

TEST(Catalog, FindsAVehicleRowById)
{
    EXPECT_EQ(kCatalog.FindVehicle("subaru-forester"), std::optional<std::size_t>(2));
    EXPECT_EQ(kCatalog.FindVehicle("absent"), std::nullopt);
    EXPECT_EQ(kCatalog.FindVehicle(""), std::nullopt);
}

TEST(Catalog, ProtocolLookupTakesTheLastMatchingRow)
{
    EXPECT_EQ(kCatalog.LastVehicleForProtocol("proto_a"), std::optional<std::size_t>(2));
    EXPECT_EQ(kCatalog.LastVehicleForProtocol("proto_b"), std::optional<std::size_t>(3));
    EXPECT_EQ(kCatalog.LastVehicleForProtocol("absent"), std::nullopt);
}

TEST(Catalog, AliasLookupTakesTheFirstMatchingVehicle)
{
    EXPECT_EQ(kCatalog.FirstVehicleForAlias("shared"), &kVehicles[1]);
    EXPECT_EQ(kCatalog.FirstVehicleForAlias("alias_a"), &kVehicles[0]);
    EXPECT_EQ(kCatalog.FirstVehicleForAlias("absent"), nullptr);
}

TEST(Catalog, AnEmptyFlashMethodMatchesNoAlias)
{
    static constexpr auto kUnaliased = std::to_array<ProtocolSpec>({{.name = "plain"}});
    static constexpr auto kOne =
        std::to_array<VehicleSpec>({{.id = "one", .protocol = ProtocolIn(kUnaliased, "plain")}});
    EXPECT_EQ((Catalog{kUnaliased, kOne}.FirstVehicleForAlias("")), nullptr);
}

TEST(CatalogProblems, AConsistentCatalogHasNone)
{
    EXPECT_THAT(CatalogProblems(kCatalog), IsEmpty());
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
    EXPECT_FALSE(CatalogReferencesResolve(kBrokenProtocols, kBrokenVehicles));
    EXPECT_THAT(CatalogProblems(Catalog{kBrokenProtocols, kBrokenVehicles}),
                UnorderedElementsAre(
                    "duplicate protocol name 'dup'", "protocol name is empty",
                    "protocol 'lonely' alias 'a,b' contains ','",
                    "protocol 'lonely' has a kernel load address but no kernel", "protocol 'lonely' has no vehicle",
                    "vehicle id 'Upper' is not lowercase [a-z0-9-]", "duplicate vehicle id 'twin'",
                    "vehicle 'orphan' has no protocol", "vehicle 'stranger' protocol is not in this catalog"));
}

} // namespace
