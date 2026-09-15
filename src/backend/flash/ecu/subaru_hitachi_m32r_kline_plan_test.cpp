#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kNormalCase{
    .name = "SubaruHitachiM32rKlineNormal",
    .build = &build_subaru_hitachi_m32r_kline_plan,
    .protocol = "sub_ecu_hitachi_m32r_kline",
    .mcu = "M32R_512KB_1block",
    .foreign_protocol = "sub_ecu_hitachi_m32r_kline_typo",
    .foreign_mcu = "M32R_512KB_4blocks",
    .read_region = MemoryRegion{.start = 0, .length = 0x80000},
    .erase_region = MemoryRegion{.start = 0, .length = 0x80000},
    .image_size = 0x80000,
};

constexpr SingleWindowPlanCase kRecoveryCase{
    .name = "SubaruHitachiM32rKlineRecovery",
    .build = &build_subaru_hitachi_m32r_kline_plan,
    .protocol = "sub_ecu_hitachi_m32r_kline_recovery",
    .mcu = "M32R_512KB_1block",
    .foreign_protocol = "sub_ecu_hitachi_m32r_kline_typo",
    .foreign_mcu = "M32R_512KB_4blocks",
    .read_region = MemoryRegion{.start = 0, .length = 0x80000},
    .erase_region = MemoryRegion{.start = 0, .length = 0x80000},
    .image_size = 0x80000,
};

// This family accepts two protocol names (Normal and Recovery session
// modes), so it is instantiated twice with distinct prefixes.
INSTANTIATE_TEST_SUITE_P(SubaruHitachiM32rKlineNormal, SingleWindowPlanContract, ::testing::Values(kNormalCase),
                         caseName);
INSTANTIATE_TEST_SUITE_P(SubaruHitachiM32rKlineRecovery, SingleWindowPlanContract, ::testing::Values(kRecoveryCase),
                         caseName);

// The session-mode mapping, transport/family identity, and wire parameters
// are this family's own; they do not generalize.
TEST(SubaruHitachiM32rKlinePlan, MapsExactProtocolsToTheirSessionModeAndWireParameters)
{
    for (const auto& [protocol, mode] : {
             std::pair{std::string_view("sub_ecu_hitachi_m32r_kline"), HitachiM32rKlineSessionMode::Normal},
             std::pair{std::string_view("sub_ecu_hitachi_m32r_kline_recovery"), HitachiM32rKlineSessionMode::Recovery},
         })
    {
        const auto plan =
            build_subaru_hitachi_m32r_kline_plan(FlashOperation::Read, protocol, "M32R_512KB_1block", std::nullopt);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_EQ(plan->family(), FlashFamily::SubaruHitachiM32rKline);
        EXPECT_EQ(plan->transport(), TransportKind::Kline);

        const auto& family = std::get<SubaruHitachiM32rKlinePlan>(plan->family_plan());
        EXPECT_EQ(family.session_mode, mode);
        EXPECT_EQ(family.tester_id, 0xf0);
        EXPECT_EQ(family.target_id, 0x10);
        EXPECT_EQ(family.initial_baud, 4800);
        EXPECT_EQ(family.write_baud, 15625);
        EXPECT_EQ(family.read_baud, 38400);
        EXPECT_EQ(family.chunk_size, 128U);
        EXPECT_EQ(family.read_address_bias, 0x100000U);
    }
}
} // namespace
} // namespace fastecu::flash::testing
