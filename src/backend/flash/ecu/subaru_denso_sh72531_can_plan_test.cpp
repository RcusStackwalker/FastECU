// subaru_denso_sh72531_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruDensoSh72531Can",
    .build = &build_subaru_denso_sh72531_can_plan,
    .protocol = "sub_ecu_denso_sh72531_can",
    .mcu = "SH72531",
    .foreign_protocol = "sub_ecu_denso_1n83m_1_5m_can",
    .foreign_mcu = "N83M_1_5MB",
    .read_region = MemoryRegion{.start = 0x00008000, .length = 0x00137F00},
    .erase_region = MemoryRegion{.start = 0x00008000, .length = 0x00137F00},
    .image_size = 0x140000,
};

INSTANTIATE_TEST_SUITE_P(SubaruDensoSh72531Can, SingleWindowPlanContract, ::testing::Values(kCase), caseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruDensoSh72531CanPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan =
        build_subaru_denso_sh72531_can_plan(FlashOperation::Read, "sub_ecu_denso_sh72531_can", "SH72531", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruDensoSh72531CanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e0U);
    EXPECT_EQ(family.response_id, 0x7e8U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_EQ(family.lead_pad_len, 0x8000U);
    EXPECT_EQ(family.tail_pad_len, 0x100U);
}
} // namespace
} // namespace fastecu::flash::testing
