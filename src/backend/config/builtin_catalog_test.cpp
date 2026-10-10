#include "src/backend/config/builtin_catalog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/memory/memory_map.h"

namespace
{

using fastecu::config::BuiltinCatalog;
using fastecu::config::CatalogProblems;
using fastecu::config::ProtocolSpec;
using fastecu::config::SelectMemoryMap;
using fastecu::memory::ByteCount;
using fastecu::memory::FileBacking;
using fastecu::memory::FileOffset;
using fastecu::memory::FillBacking;
using fastecu::memory::FlashAddress;
using fastecu::memory::MemoryBlock;
using fastecu::memory::MemoryErrorKind;
using fastecu::memory::MemoryMap;
using fastecu::memory::Writability;
using Backing = std::variant<FileBacking, FillBacking>;
using ::testing::IsEmpty;

const ProtocolSpec& ProtocolNamed(std::string_view name)
{
    const ProtocolSpec *protocol = BuiltinCatalog().FindProtocol(name);
    if (protocol == nullptr)
    {
        ADD_FAILURE() << "no built-in protocol " << name;
        static constexpr ProtocolSpec kAbsent{};
        return kAbsent;
    }
    return *protocol;
}

TEST(BuiltinCatalogFixes, Sh7055TcuKernelNamesTheBundledFile)
{
    EXPECT_EQ(ProtocolNamed("sub_tcu_denso_sh7055_can").kernel, "ssmk_tcu_can_sh7055_35.bin");
}

TEST(BuiltinCatalogFixes, Legacy1990RowsReachTheRenamedUnisiaJecsProtocols)
{
    const auto vehicles = BuiltinCatalog().Vehicles();
    ASSERT_GE(vehicles.size(), 3U);
    ASSERT_NE(vehicles[1].protocol, nullptr);
    ASSERT_NE(vehicles[2].protocol, nullptr);
    EXPECT_EQ(vehicles[1].protocol->name, "sub_ecu_unisia_jecs_m3779x");
    EXPECT_EQ(vehicles[2].protocol->name, "sub_ecu_unisia_jecs_m3775x");
}

std::string_view Trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

TEST(BuiltinCatalogFixes, VehicleTextHasNoStrayWhitespace)
{
    for (const auto& vehicle : BuiltinCatalog().Vehicles())
    {
        for (std::string_view field : {vehicle.make, vehicle.model, vehicle.version, vehicle.type, vehicle.kw,
                                       vehicle.hp, vehicle.fuel, vehicle.year})
        {
            EXPECT_EQ(field, Trimmed(field)) << vehicle.id;
        }
    }
}

TEST(BuiltinCatalogFixes, Sh7059DieselDensoCanYearIs2011)
{
    const auto row = BuiltinCatalog().FindVehicle(
        "subaru-all-sh7059-denso-can-diesel-models-sh7059-2011--sub-ecu-denso-sh7059-diesel-densocan");
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(BuiltinCatalog().Vehicles()[*row].year, "2011");
}

TEST(BuiltinCatalogFixes, UnisiaJecsM377xDeclareNoKernelLoadAddress)
{
    for (std::string_view name : {"sub_ecu_unisia_jecs_m3779x", "sub_ecu_unisia_jecs_m3775x"})
    {
        EXPECT_TRUE(ProtocolNamed(name).kernel.empty()) << name;
        EXPECT_FALSE(ProtocolNamed(name).kernel_load_address.has_value()) << name;
    }
}

TEST(BuiltinCatalogFixes, Sh72543DieselUploadsNoKernel)
{
    const ProtocolSpec& protocol = ProtocolNamed("sub_ecu_denso_sh72543_can_diesel");
    EXPECT_TRUE(protocol.kernel.empty());
    EXPECT_FALSE(protocol.kernel_load_address.has_value());
}

TEST(BuiltinCatalogFixes, Sh7058DensoCanEepromLoadsItsKernelWhereTheFlashFamilyDoes)
{
    const ProtocolSpec& eeprom = ProtocolNamed("sub_ecu_eeprom_denso_sh7058_densocan");
    const ProtocolSpec& flash = ProtocolNamed("sub_ecu_denso_sh7058_densocan");
    EXPECT_EQ(eeprom.kernel, flash.kernel);
    EXPECT_EQ(eeprom.kernel_load_address, std::optional<std::uint32_t>(0xFFFF3000U));
    EXPECT_EQ(eeprom.kernel_load_address, flash.kernel_load_address);
}

TEST(BuiltinCatalog, ListsSixtyOneProtocolsAndSeventyFiveVehicles)
{
    EXPECT_EQ(BuiltinCatalog().Protocols().size(), 61U);
    EXPECT_EQ(BuiltinCatalog().Vehicles().size(), 75U);
}

TEST(BuiltinCatalog, IsConsistent)
{
    EXPECT_THAT(CatalogProblems(BuiltinCatalog()), IsEmpty());
}

TEST(BuiltinCatalogFixes, Mc68Revision04IsGone)
{
    EXPECT_EQ(BuiltinCatalog().FindProtocol("sub_ecu_denso_mc68hc16y5_04"), nullptr);
    EXPECT_EQ(BuiltinCatalog().FindProtocol("sub_ecu_denso_mc68hc16y5_04_ecutek"), nullptr);
}

// The new vehicles come after every older row, so an alias two protocols
// share still resolves to the protocol it resolved to before.
TEST(BuiltinCatalogFixes, SharedAliasesResolveAsBefore)
{
    const auto resolves_to = [](std::string_view alias)
    {
        const auto *vehicle = BuiltinCatalog().FirstVehicleForAlias(alias);
        return vehicle == nullptr ? std::string_view{} : vehicle->protocol->name;
    };
    EXPECT_EQ(resolves_to("fxt02"), "sub_ecu_denso_sh7055_02");
    EXPECT_EQ(resolves_to("wrx02"), "sub_ecu_denso_mc68hc16y5_02");
    EXPECT_EQ(resolves_to("subarucand"), "sub_ecu_denso_sh7058_can_diesel");
}

TEST(BuiltinCatalogFixes, CobbAliasesNowSelectTheirVehicles)
{
    for (const auto& [alias, protocol] :
         {std::pair{"sti04_cobb", "sub_ecu_denso_sh7055_04_cobb"}, std::pair{"sti05_cobb", "sub_ecu_denso_sh7058_cobb"},
          std::pair{"subarucan_cobb", "sub_ecu_denso_sh7058_can_cobb"}})
    {
        const auto *vehicle = BuiltinCatalog().FirstVehicleForAlias(alias);
        ASSERT_NE(vehicle, nullptr) << alias;
        EXPECT_EQ(vehicle->protocol->name, protocol);
    }
}

TEST(BuiltinCatalogFixes, MutDmaLoggingHasAMitsubishiVehicle)
{
    const auto row = BuiltinCatalog().FindVehicle("mitsubishi-unknown-unk-ecu-unk--mitsu-ecu-m32r-kline-mut-dma");
    ASSERT_TRUE(row.has_value());
    const auto& vehicle = BuiltinCatalog().Vehicles()[*row];
    EXPECT_EQ(vehicle.make, "Mitsubishi");
    EXPECT_EQ(vehicle.protocol->log_protocol, "MUT_DMA");
}

// The memory map `protocol` gives a `file_size`-byte ROM file; a test failure
// when it gives none.
std::optional<MemoryMap> MapFor(std::string_view protocol, std::uint32_t file_size)
{
    auto map = SelectMemoryMap(ProtocolNamed(protocol), ByteCount{file_size});
    if (!map.has_value())
    {
        ADD_FAILURE() << protocol << ": " << map.error().detail;
        return std::nullopt;
    }
    return std::move(*map);
}

constexpr std::array<std::string_view, 3> kMc68Protocols{
    "sub_ecu_denso_mc68hc16y5_02", "sub_ecu_denso_mc68hc16y5_02_ecutek", "sub_ecu_denso_mc68hc16y5_02_bdm"};

// ApplyFlashMethodPadding inserts 0x8000 bytes of 0xFF at 0x20000 into a
// 160 KiB file; the packed map reads the same bytes there without inserting any.
TEST(BuiltinCatalogMemoryMaps, Mc68PackedFilesReadErasedFlashInTheRamRange)
{
    for (const std::string_view protocol : kMc68Protocols)
    {
        const std::optional<MemoryMap> map = MapFor(protocol, 0x28000);
        ASSERT_TRUE(map.has_value());
        const MemoryBlock *ram = map->BlockAt(FlashAddress{0x20000});
        const MemoryBlock *high = map->BlockAt(FlashAddress{0x28000});
        ASSERT_NE(ram, nullptr) << protocol;
        ASSERT_NE(high, nullptr) << protocol;
        EXPECT_EQ(ram->backing, Backing{FillBacking{.value = 0xFF}}) << protocol;
        EXPECT_EQ(ram->range.End(), FlashAddress{0x28000}) << protocol;
        EXPECT_EQ(high->backing, Backing{FileBacking{.offset = FileOffset{0x20000}}}) << protocol;
        EXPECT_EQ(high->writability, Writability::kWritable) << protocol;
    }
}

// A BDM read, or a community file declaring `filesize 192kb`.
TEST(BuiltinCatalogMemoryMaps, Mc68FullFilesHoldReadOnlyBytesForTheRamRange)
{
    for (const std::string_view protocol : kMc68Protocols)
    {
        const std::optional<MemoryMap> map = MapFor(protocol, 0x30000);
        ASSERT_TRUE(map.has_value());
        const MemoryBlock *ram = map->BlockAt(FlashAddress{0x20000});
        const MemoryBlock *high = map->BlockAt(FlashAddress{0x28000});
        ASSERT_NE(ram, nullptr) << protocol;
        ASSERT_NE(high, nullptr) << protocol;
        EXPECT_EQ(ram->backing, Backing{FileBacking{.offset = FileOffset{0x20000}}}) << protocol;
        EXPECT_EQ(ram->writability, Writability::kReadOnly) << protocol;
        EXPECT_EQ(high->backing, Backing{FileBacking{.offset = FileOffset{0x28000}}}) << protocol;
        EXPECT_EQ(high->writability, Writability::kWritable) << protocol;
    }
}

TEST(BuiltinCatalogMemoryMaps, Mc68FilesOfOtherSizesAreRejectedNotPadded)
{
    for (const std::uint32_t file_size : {0x20000U, 0x40000U})
    {
        const auto map = SelectMemoryMap(ProtocolNamed("sub_ecu_denso_mc68hc16y5_02"), ByteCount{file_size});
        ASSERT_FALSE(map.has_value()) << file_size;
        EXPECT_EQ(map.error().kind, MemoryErrorKind::kFileSizeMismatch) << file_size;
    }
}

TEST(BuiltinCatalogMemoryMaps, N83mFilesStartAtTheirImageAddress)
{
    struct N83mFile
    {
        std::string_view protocol;
        std::uint32_t file_size;
    };
    for (const N83mFile file :
         {N83mFile{"sub_ecu_denso_1n83m_4m_can", 0x3E4000}, N83mFile{"sub_ecu_denso_1n83m_1_5m_can", 0x184000}})
    {
        const std::optional<MemoryMap> map = MapFor(file.protocol, file.file_size);
        ASSERT_TRUE(map.has_value());
        const MemoryBlock *lead = map->BlockAt(FlashAddress{0x08F9C000});
        const MemoryBlock *main = map->BlockAt(FlashAddress{0x08FAC000});
        EXPECT_EQ(map->BlockAt(FlashAddress{0}), nullptr) << file.protocol;
        ASSERT_NE(lead, nullptr) << file.protocol;
        ASSERT_NE(main, nullptr) << file.protocol;
        EXPECT_EQ(lead->backing, Backing{FileBacking{.offset = FileOffset{0}}}) << file.protocol;
        EXPECT_EQ(lead->writability, Writability::kReadOnly) << file.protocol;
        EXPECT_EQ(main->backing, Backing{FileBacking{.offset = FileOffset{0x10000}}}) << file.protocol;
        EXPECT_EQ(main->writability, Writability::kWritable) << file.protocol;
    }
}

// Q13: writable means the protocol can write the block by any route; the
// 512 KiB Colt variants reach 0x60000-0x7FFFF through the redirect carrier.
TEST(BuiltinCatalogMemoryMaps, ColtFullFilesWriteTheTopRegionButNotTheBootloader)
{
    const std::optional<MemoryMap> full = MapFor("mitsu_ecu_m32r_can_512kb", 0x80000);
    const std::optional<MemoryMap> userspace = MapFor("mitsu_ecu_m32r_can", 0x60000);
    ASSERT_TRUE(full.has_value());
    ASSERT_TRUE(userspace.has_value());

    ASSERT_NE(full->BlockAt(FlashAddress{0x7FFF}), nullptr);
    ASSERT_NE(full->BlockAt(FlashAddress{0x60000}), nullptr);
    EXPECT_EQ(full->BlockAt(FlashAddress{0x7FFF})->writability, Writability::kReadOnly);
    EXPECT_EQ(full->BlockAt(FlashAddress{0x60000})->writability, Writability::kWritable);
    EXPECT_EQ(userspace->BlockAt(FlashAddress{0x60000}), nullptr);
}

TEST(BuiltinCatalogMemoryMaps, ProtocolsDeclaringNoMapGetTheIdentityMapAtAnySize)
{
    for (const std::uint32_t file_size : {0x100000U, 0x12345U})
    {
        const std::optional<MemoryMap> map = MapFor("sub_ecu_denso_sh7058", file_size);
        ASSERT_TRUE(map.has_value());
        ASSERT_EQ(map->Blocks().size(), 1U) << file_size;
        EXPECT_EQ(map->Blocks()[0].range.Start(), FlashAddress{0}) << file_size;
        EXPECT_EQ(map->Blocks()[0].range.Size(), ByteCount{file_size}) << file_size;
        EXPECT_EQ(map->Blocks()[0].writability, Writability::kWritable) << file_size;
    }
}

// ADR 0020: full-size community files and FastECU's own reads share one memory
// map, so every ROM file a family's read produces opens with its protocol's map.
TEST(BuiltinCatalogMemoryMaps, FastEcuReadFilesOpenWithTheirProtocolsMap)
{
    struct ReadFile
    {
        std::string_view protocol;
        std::uint32_t file_size;
    };
    for (const ReadFile file : std::to_array<ReadFile>({
             {"sub_ecu_denso_mc68hc16y5_02", 0x28000},
             {"sub_ecu_denso_mc68hc16y5_02_ecutek", 0x28000},
             {"sub_ecu_denso_mc68hc16y5_02_bdm", 0x30000},
             {"sub_ecu_denso_sh72543_can_diesel", 0x200000},
             {"sub_tcu_hitachi_m32r_can", 0x80000},
             {"sub_tcu_cvt_hitachi_m32r_can", 0x80000},
             {"sub_tcu_cvt_mitsu_mh8104_can", 0x80000},
             {"sub_tcu_cvt_mitsu_mh8111_can", 0x80000},
             {"sub_ecu_mitsu_m32r_kline", 0x80000},
             {"mitsu_ecu_m32r_can", 0x60000},
             {"mitsu_ecu_m32r_can_vendor_ext", 0x60000},
             {"mitsu_ecu_m32r_can_512kb", 0x80000},
             {"mitsu_ecu_m32r_can_vendor_ext_512kb", 0x80000},
             {"sub_ecu_hitachi_sh72543r_can", 0x200000},
             {"sub_ecu_hitachi_sh72543r_can_recovery", 0x200000},
             {"sub_ecu_denso_sh72531_can", 0x140000},
             {"sub_ecu_denso_1n83m_4m_can", 0x3E4000},
             {"sub_ecu_denso_1n83m_1_5m_can", 0x184000},
         }))
    {
        EXPECT_TRUE(MapFor(file.protocol, file.file_size).has_value()) << file.protocol;
    }
}

// The MH8111 read covers 0x8000-0x7FFFF; the protocol writes only from 0x80000,
// so nothing in a read file can be edited.
TEST(BuiltinCatalogMemoryMaps, Mh8111ReadFilesHoldNoWritableBytes)
{
    const std::optional<MemoryMap> map = MapFor("sub_tcu_cvt_mitsu_mh8111_can", 0x80000);
    ASSERT_TRUE(map.has_value());
    EXPECT_THAT(map->Blocks(), ::testing::Each(::testing::Field(&MemoryBlock::writability, Writability::kReadOnly)));
}

} // namespace
