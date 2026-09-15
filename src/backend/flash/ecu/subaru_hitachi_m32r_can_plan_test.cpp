// subaru_hitachi_m32r_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruHitachiM32rCan",
    .build = &build_subaru_hitachi_m32r_can_plan,
    .protocol = "sub_ecu_hitachi_m32r_can",
    .mcu = "M32R_512KB_1block",
    .foreign_protocol = "sub_ecu_hitachi_m32r_can_typo",
    .foreign_mcu = "MH8104",
    .read_region = MemoryRegion{.start = 0, .length = 0x80000},
    .erase_region = MemoryRegion{.start = 0, .length = 0x80000},
    .image_size = 0x80000,
};

INSTANTIATE_TEST_SUITE_P(SubaruHitachiM32rCan, SingleWindowPlanContract, ::testing::Values(kCase), caseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruHitachiM32rCanPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Read, "sub_ecu_hitachi_m32r_can",
                                                         "M32R_512KB_1block", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruHitachiM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e0U);
    EXPECT_EQ(family.response_id, 0x7e8U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
}
} // namespace
} // namespace fastecu::flash::testing
