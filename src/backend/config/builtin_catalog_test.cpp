#include "src/backend/config/builtin_catalog.h"

#include <array>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::config::builtin_catalog;
using fastecu::config::catalog_problems;
using ::testing::UnorderedElementsAreArray;

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
    "protocol 'sub_ecu_unisia_jecs_m3779x' has no vehicle",
    "protocol 'sub_ecu_unisia_jecs_m3775x' has a kernel load address but no kernel",
    "protocol 'sub_ecu_unisia_jecs_m3775x' has no vehicle",
    "vehicle 'subaru-legacy-2-0-a-t-1990--sub-ecu-unisia-jecs-m3779x' has no protocol",
    "vehicle 'subaru-legacy-2-0-a-t-1990--sub-ecu-unisia-jecs-m3775x' has no protocol",
});

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
