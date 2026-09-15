// subaru_denso_1n83m_1_5m_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruDenso1n83m_1_5mCan",
    .build = &build_subaru_denso_1n83m_1_5m_can_plan,
    .protocol = "sub_ecu_denso_1n83m_1_5m_can",
    .mcu = "N83M_1_5MB",
    .foreign_protocol = "sub_ecu_denso_sh72531_can",
    .foreign_mcu = "SH72531",
    .read_region = MemoryRegion{.start = 0x08FAC000, .length = 0x00173F00},
    .erase_region = MemoryRegion{.start = 0x08FAC000, .length = 0x00173F00},
    .image_size = 0x184000,
};

INSTANTIATE_TEST_SUITE_P(SubaruDenso1n83m_1_5mCan, SingleWindowPlanContract, ::testing::Values(kCase), caseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruDenso1n83m_1_5mCanPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Read, "sub_ecu_denso_1n83m_1_5m_can",
                                                             "N83M_1_5MB", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruDenso1n83m_1_5mCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e0U);
    EXPECT_EQ(family.response_id, 0x7e8U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_EQ(family.lead_pad_len, 0x10000U);
    EXPECT_EQ(family.tail_pad_len, 0x100U);
}
} // namespace
} // namespace fastecu::flash::testing
