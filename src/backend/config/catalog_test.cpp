#include "src/backend/config/catalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/protocol/bytes.h"

namespace
{

using fastecu::config::Catalog;
using fastecu::config::CatalogProblems;
using fastecu::config::CatalogReferencesResolve;
using fastecu::config::ChecksumFlag;
using fastecu::config::ChecksumSupport;
using fastecu::config::FileBlock;
using fastecu::config::KernelLoadAddressText;
using fastecu::config::MemoryMapSpec;
using fastecu::config::PlaceRomFile;
using fastecu::config::ProtocolIn;
using fastecu::config::ProtocolSpec;
using fastecu::config::SelectMemoryMap;
using fastecu::config::VehicleSpec;
using fastecu::memory::ByteCount;
using fastecu::memory::FileBacking;
using fastecu::memory::FileOffset;
using fastecu::memory::FlashAddress;
using fastecu::memory::MemoryErrorKind;
using fastecu::memory::Writability;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
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

// A 0x200-byte file whose first 0x100 bytes the protocol never writes, and a
// 0x100-byte file of only the writable half, placed where the larger file puts it.
constexpr auto kPrefixedBlocks = std::to_array({
    FileBlock(0x000, 0x100, 0x000, Writability::kReadOnly),
    FileBlock(0x100, 0x100, 0x100, Writability::kWritable),
});
constexpr auto kTailBlocks = std::to_array({FileBlock(0x100, 0x100, 0x000, Writability::kWritable)});
constexpr auto kMaps = std::to_array<MemoryMapSpec>({
    {.file_size = ByteCount{0x200}, .blocks = kPrefixedBlocks},
    {.file_size = ByteCount{0x100}, .blocks = kTailBlocks},
});
constexpr ProtocolSpec kMapped{.name = "mapped", .memory_maps = kMaps};
constexpr ProtocolSpec kPlain{.name = "plain"};

TEST(SelectMemoryMap, GivesTheIdentityMapWhenTheProtocolDeclaresNone)
{
    const auto map = SelectMemoryMap(kPlain, ByteCount{0x1234});

    ASSERT_TRUE(map.has_value());
    ASSERT_EQ(map->Blocks().size(), 1U);
    EXPECT_EQ(map->Blocks()[0].range.Start(), FlashAddress{0});
    EXPECT_EQ(map->Blocks()[0].range.Size(), ByteCount{0x1234});
    EXPECT_EQ(map->Blocks()[0].writability, Writability::kWritable);
}

TEST(SelectMemoryMap, PicksTheDeclaredMapOfExactlyTheFileSize)
{
    const auto large = SelectMemoryMap(kMapped, ByteCount{0x200});
    const auto small = SelectMemoryMap(kMapped, ByteCount{0x100});

    ASSERT_TRUE(large.has_value());
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(large->Blocks().size(), 2U);
    ASSERT_EQ(small->Blocks().size(), 1U);
    EXPECT_EQ(small->Blocks()[0].range.Start(), FlashAddress{0x100});
    EXPECT_EQ(small->BlockAt(FlashAddress{0x100})->backing,
              (std::variant<FileBacking, fastecu::memory::FillBacking>{FileBacking{.offset = FileOffset{0}}}));
}

TEST(SelectMemoryMap, RejectsAFileSizeNoDeclaredMapHas)
{
    const auto map = SelectMemoryMap(kMapped, ByteCount{0x180});

    ASSERT_FALSE(map.has_value());
    EXPECT_EQ(map.error().kind, MemoryErrorKind::kFileSizeMismatch);
}

// 0x100 bytes of blocks for a 0x200-byte file; two maps claim 0x80-byte files.
constexpr auto kShortBlocks = std::to_array({FileBlock(0x0, 0x100, 0x0, Writability::kWritable)});
constexpr auto kSmallBlocks = std::to_array({FileBlock(0x0, 0x80, 0x0, Writability::kWritable)});
constexpr auto kBadMaps = std::to_array<MemoryMapSpec>({
    {.file_size = ByteCount{0x200}, .blocks = kShortBlocks},
    {.file_size = ByteCount{0x80}, .blocks = kSmallBlocks},
    {.file_size = ByteCount{0x80}, .blocks = kSmallBlocks},
});
constexpr auto kBadMapProtocols = std::to_array<ProtocolSpec>({{.name = "badmaps", .memory_maps = kBadMaps}});
constexpr auto kBadMapVehicles =
    std::to_array<VehicleSpec>({{.id = "car", .protocol = ProtocolIn(kBadMapProtocols, "badmaps")}});

TEST(CatalogProblems, ReportsInvalidAndSameSizeMemoryMaps)
{
    EXPECT_THAT(CatalogProblems(Catalog{kBadMapProtocols, kBadMapVehicles}),
                ElementsAre("protocol 'badmaps' memory map for 0x200-byte files is invalid: "
                            "ROM file bytes from 0x100 are not placed",
                            "protocol 'badmaps' declares two memory maps for 0x80-byte files"));
}

// A 0x100-byte file at 0x1000 whose definitions count from 0x1000.
constexpr auto kBasedBlocks = std::to_array({FileBlock(0x1000, 0x100, 0x0, Writability::kWritable)});
constexpr auto kBasedMaps = std::to_array<MemoryMapSpec>(
    {{.file_size = ByteCount{0x100}, .blocks = kBasedBlocks, .definition_base = FlashAddress{0x1000}}});
constexpr ProtocolSpec kBased{.name = "based", .memory_maps = kBasedMaps};

TEST(SelectMemoryMap, CarriesTheDeclaredDefinitionBase)
{
    const auto based = SelectMemoryMap(kBased, ByteCount{0x100});
    const auto plain = SelectMemoryMap(kPlain, ByteCount{0x100});

    ASSERT_TRUE(based.has_value());
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(based->DefinitionBase(), FlashAddress{0x1000});
    EXPECT_EQ(plain->DefinitionBase(), FlashAddress{0});
}

TEST(SelectMemoryMapForAFile, GivesTheIdentityMapWithoutAProtocol)
{
    const auto map = SelectMemoryMap(nullptr, 0x40);

    ASSERT_TRUE(map.has_value());
    ASSERT_EQ(map->Blocks().size(), 1U);
    EXPECT_EQ(map->Blocks()[0].range.Start(), FlashAddress{0});
    EXPECT_EQ(map->FileSize(), ByteCount{0x40});
}

TEST(SelectMemoryMapForAFile, RejectsAnEmptyFile)
{
    for (const ProtocolSpec *protocol : {static_cast<const ProtocolSpec *>(nullptr), &kPlain, &kMapped})
    {
        const auto map = SelectMemoryMap(protocol, 0);

        ASSERT_FALSE(map.has_value());
        EXPECT_EQ(map.error().kind, MemoryErrorKind::kFileSizeMismatch);
        EXPECT_EQ(map.error().detail, "the ROM file is empty");
    }
}

TEST(SelectMemoryMapForAFile, RejectsAFileOver4GiB)
{
    const auto map = SelectMemoryMap(&kPlain, std::size_t{1} << 32);

    ASSERT_FALSE(map.has_value());
    EXPECT_EQ(map.error().kind, MemoryErrorKind::kFileSizeMismatch);
    EXPECT_EQ(map.error().detail, "the 4294967296-byte ROM file is larger than 4 GiB");
}

TEST(PlaceRomFile, PlacesTheFileByItsProtocolsMap)
{
    bytes::Bytes file(0x100, 0x00);
    file[0] = 0xAB;

    const auto image = PlaceRomFile(&kMapped, file);

    ASSERT_TRUE(image.has_value());
    EXPECT_THAT(image->File(), ElementsAreArray(file));
    const auto view = image->Render(
        fastecu::memory::AddressRange<fastecu::memory::FlashSpace>::Make(FlashAddress{0x100}, ByteCount{1}).value());
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->Data()[0], 0xAB);
}

TEST(PlaceRomFile, CarriesTheSelectionError)
{
    const auto image = PlaceRomFile(&kMapped, bytes::Bytes(0x180, 0x00));

    ASSERT_FALSE(image.has_value());
    EXPECT_EQ(image.error().detail, "protocol 'mapped' has no memory map for 0x180-byte ROM files");
}

} // namespace
