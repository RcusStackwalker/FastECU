#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kReadCase{
    .name = "SubaruTcuHitachiM32rKline",
    .build = &build_subaru_tcu_hitachi_m32r_kline_plan,
    .protocol = "sub_tcu_hitachi_m32r_kline",
    .mcu = "M32R_512KB",
    .foreign_protocol = "sub_tcu_hitachi_m32r_kline_typo",
    .foreign_mcu = "M32R_512KB_1block",
    .read_region = MemoryRegion{.start = 0, .length = 0x80000},
    .erase_region = MemoryRegion{.start = 0, .length = 0x80000},
    .image_size = 0x80000,
    .supports_write = false,
};

INSTANTIATE_TEST_SUITE_P(SubaruTcuHitachiM32rKline, SingleWindowPlanContract, ::testing::Values(kReadCase), caseName);

TEST(SubaruTcuHitachiM32rKlinePlan, MapsProtocolToItsWireParameters)
{
    const auto plan = build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_tcu_hitachi_m32r_kline",
                                                               "M32R_512KB", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->family(), FlashFamily::SubaruTcuHitachiM32rKline);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);

    const auto& family = std::get<SubaruTcuHitachiM32rKlinePlan>(plan->family_plan());
    EXPECT_EQ(family.tester_id, 0xf0);
    EXPECT_EQ(family.target_id, 0x18);
    EXPECT_EQ(family.baud, 4800);
    EXPECT_EQ(family.block_size, 96u);
}

// Deliberate divergence 1: the legacy write branch reported success having
// written nothing. See the plan's "Deliberate Divergences From Legacy".
TEST(SubaruTcuHitachiM32rKlinePlan, RejectsWriteAndTestWriteAsUnsupported)
{
    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        const auto plan = build_subaru_tcu_hitachi_m32r_kline_plan(operation, "sub_tcu_hitachi_m32r_kline",
                                                                   "M32R_512KB", bytes::Bytes(0x80000, 0x00));
        EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::Unsupported));
    }
}
} // namespace
} // namespace fastecu::flash::testing
