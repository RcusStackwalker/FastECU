// subaru_tcu_cvt_hitachi_m32r_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruTcuCvtHitachiM32rCan",
    .build = &build_subaru_tcu_cvt_hitachi_m32r_can_plan,
    .protocol = "sub_tcu_cvt_hitachi_m32r_can",
    .mcu = "M32R_512KB",
    .foreign_protocol = "sub_tcu_cvt_hitachi_m32r_can_typo",
    .foreign_mcu = "MH8104",
    // Legacy read_mem computes start_addr - 0x00100000 with start_addr == 0,
    // which underflows uint32_t to 0xFFF00000 and bypasses the "< 0x8000"
    // floor clamp entirely -- this path never executed in production
    // (execute() called hack_words(), never read_mem()). This plan targets
    // the clamp's evident intent (0x8000) rather than reproducing an address
    // computation nothing ever observed on the wire.
    .read_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .erase_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .image_size = 0x80000,
};

INSTANTIATE_TEST_SUITE_P(SubaruTcuCvtHitachiM32rCan, SingleWindowPlanContract, ::testing::Values(kCase), caseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruTcuCvtHitachiM32rCanPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Read, "sub_tcu_cvt_hitachi_m32r_can",
                                                                 "M32R_512KB", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruTcuCvtHitachiM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e1U);
    EXPECT_EQ(family.response_id, 0x7e9U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
}
} // namespace
} // namespace fastecu::flash::testing
