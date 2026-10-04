#include "src/backend/config/builtin_catalog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::config::builtin_catalog;
using fastecu::config::catalog_problems;
using fastecu::config::ProtocolSpec;
using ::testing::UnorderedElementsAreArray;

const ProtocolSpec& protocol_named(std::string_view name)
{
    const ProtocolSpec *protocol = builtin_catalog().find_protocol(name);
    if (protocol == nullptr)
    {
        ADD_FAILURE() << "no built-in protocol " << name;
        static constexpr ProtocolSpec kAbsent{};
        return kAbsent;
    }
    return *protocol;
}

// Every inconsistency protocols.cfg carried. Each data fix removes its lines;
// the reachability fix (Task 5) empties the list.
constexpr auto kKnownDefects = std::to_array<std::string_view>({
    "protocol 'sub_ecu_denso_mc68hc16y5_04' has no vehicle",
    "protocol 'sub_ecu_denso_mc68hc16y5_04_ecutek' has no vehicle",
    "protocol 'sub_ecu_denso_sh7055_02_ecutek' has no vehicle",
    "protocol 'sub_ecu_denso_sh7055_04_cobb' has no vehicle",
    "protocol 'sub_ecu_denso_sh7058_cobb' has no vehicle",
    "protocol 'sub_ecu_denso_sh7058_can_cobb' has no vehicle",
    "protocol 'mitsu_ecu_m32r_kline_mut_dma' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7055_kline' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7058_kline' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7055_densocan' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7058_densocan' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7058_can_diesel' has no vehicle",
    "protocol 'sub_ecu_unisia_jecs_m3779x' has a kernel load address but no kernel",
    "protocol 'sub_ecu_unisia_jecs_m3775x' has a kernel load address but no kernel",
});

TEST(BuiltinCatalogFixes, Sh7055TcuKernelNamesTheBundledFile)
{
    EXPECT_EQ(protocol_named("sub_tcu_denso_sh7055_can").kernel, "ssmk_tcu_can_sh7055_35.bin");
}

TEST(BuiltinCatalogFixes, Legacy1990RowsReachTheRenamedUnisiaJecsProtocols)
{
    const auto vehicles = builtin_catalog().vehicles();
    ASSERT_GE(vehicles.size(), 3U);
    ASSERT_NE(vehicles[1].protocol, nullptr);
    ASSERT_NE(vehicles[2].protocol, nullptr);
    EXPECT_EQ(vehicles[1].protocol->name, "sub_ecu_unisia_jecs_m3779x");
    EXPECT_EQ(vehicles[2].protocol->name, "sub_ecu_unisia_jecs_m3775x");
}

std::string_view trimmed(std::string_view text)
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
    for (const auto& vehicle : builtin_catalog().vehicles())
    {
        for (std::string_view field : {vehicle.make, vehicle.model, vehicle.version, vehicle.type, vehicle.kw,
                                       vehicle.hp, vehicle.fuel, vehicle.year})
        {
            EXPECT_EQ(field, trimmed(field)) << vehicle.id;
        }
    }
}

TEST(BuiltinCatalogFixes, Sh7059DieselDensoCanYearIs2011)
{
    const auto row = builtin_catalog().find_vehicle(
        "subaru-all-sh7059-denso-can-diesel-models-sh7059-2011--sub-ecu-denso-sh7059-diesel-densocan");
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(builtin_catalog().vehicles()[*row].year, "2011");
}

TEST(BuiltinCatalog, ListsEveryProtocolsCfgEntry)
{
    EXPECT_EQ(builtin_catalog().protocols().size(), 63U);
    EXPECT_EQ(builtin_catalog().vehicles().size(), 65U);
}

TEST(BuiltinCatalog, HasExactlyTheKnownDefects)
{
    EXPECT_THAT(catalog_problems(builtin_catalog()), UnorderedElementsAreArray(kKnownDefects));
}

} // namespace
