#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruMitsuM32rKline",
    .build = &BuildSubaruMitsuM32rKlinePlan,
    .protocol = "sub_ecu_mitsu_m32r_kline",
    .mcu = "M32R_512KB_4blocks",
    .foreign_protocol = "sub_ecu_mitsu_m32r_kline_typo",
    .foreign_mcu = "NOT_A_REAL_MCU",
    .read_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .erase_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .image_size = 0x80000,
};

INSTANTIATE_TEST_SUITE_P(SubaruMitsuM32rKline, SingleWindowPlanContract, ::testing::Values(kCase), CaseName);

// The transport/family identity and wire parameters are this family's own;
// they do not generalize.
TEST(SubaruMitsuM32rKlinePlan, ReadPlanCarriesThisFamilysIdentityAndWireParameters)
{
    const auto plan = BuildSubaruMitsuM32rKlinePlan(FlashOperation::kRead, "sub_ecu_mitsu_m32r_kline",
                                                    "M32R_512KB_4blocks", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->Family(), FlashFamily::kSubaruMitsuM32rKline);
    EXPECT_EQ(plan->Transport(), TransportKind::kKline);

    const auto& family = std::get<SubaruMitsuM32rKlinePlan>(plan->FamilyPlan());
    EXPECT_EQ(family.tester_id, 0xf0);
    EXPECT_EQ(family.target_id, 0x10);
    EXPECT_EQ(family.initial_baud, 4800);
    EXPECT_EQ(family.flash_baud, 15625);
    EXPECT_EQ(family.chunk_size, 128U);
    EXPECT_EQ(family.unread_prefix_fill, 0xff);
}
} // namespace
} // namespace fastecu::flash::testing
