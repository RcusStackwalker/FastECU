#include "src/backend/config/builtin_catalog.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::config::BuiltinCatalog;
using fastecu::config::CatalogProblems;
using fastecu::config::ProtocolSpec;
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

} // namespace
