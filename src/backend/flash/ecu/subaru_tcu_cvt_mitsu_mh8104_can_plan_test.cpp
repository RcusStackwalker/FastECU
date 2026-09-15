// subaru_tcu_cvt_mitsu_mh8104_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruTcuCvtMitsuMh8104Can",
    .build = &build_subaru_tcu_cvt_mitsu_mh8104_can_plan,
    .protocol = "sub_tcu_cvt_mitsu_mh8104_can",
    .mcu = "MH8104",
    .foreign_protocol = "sub_tcu_cvt_mitsu_mh8104_can_typo",
    .foreign_mcu = "MH8111",
    // MH8104's flash geometry is {0,0x4000},{0x4000,0x2000},{0x6000,0x2000},
    // {0x8000,0x78000} (kernelmemorymodels.h fblocks_MH8104); block_modified
    // skips blocks 0-2, so write_mem's only reflash_block call targets block
    // 3: {0x8000, 0x78000}. read_mem hardcodes the SAME {0x8000, 0x78000}
    // window -- unlike MH8111, this family's read window and its sole
    // flashed block coincide exactly, hence the equal read/erase regions
    // below.
    .read_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .erase_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .image_size = 0x80000,
};

INSTANTIATE_TEST_SUITE_P(SubaruTcuCvtMitsuMh8104Can, SingleWindowPlanContract, ::testing::Values(kCase), caseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruTcuCvtMitsuMh8104CanPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Read, "sub_tcu_cvt_mitsu_mh8104_can",
                                                                 "MH8104", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruTcuCvtMitsuMh8104CanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e1U);
    EXPECT_EQ(family.response_id, 0x7e9U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
}
} // namespace
} // namespace fastecu::flash::testing
