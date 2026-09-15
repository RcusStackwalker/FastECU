// subaru_denso_sh72543_can_diesel_plan_test.cpp
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruDensoSh72543CanDiesel",
    .build = &build_subaru_denso_sh72543_can_diesel_plan,
    .protocol = "sub_ecu_denso_sh72543_can_diesel",
    .mcu = "SH72543d",
    .foreign_protocol = "sub_ecu_denso_sh72531_can",
    .foreign_mcu = "SH72531",
    // fblocks_SH72543d has numblocks == 1 with fblocks[0] == {0x8000, 0x1F7F00};
    // this family is the only one in the wave with a single-block flash table.
    .read_region = MemoryRegion{.start = 0x00008000, .length = 0x001F7F00},
    .erase_region = MemoryRegion{.start = 0x00008000, .length = 0x001F7F00},
    .image_size = 0x200000,
};

INSTANTIATE_TEST_SUITE_P(SubaruDensoSh72543CanDiesel, SingleWindowPlanContract, ::testing::Values(kCase), caseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruDensoSh72543CanDieselPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan = build_subaru_denso_sh72543_can_diesel_plan(
        FlashOperation::Read, "sub_ecu_denso_sh72543_can_diesel", "SH72543d", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruDensoSh72543CanDieselPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e0U);
    EXPECT_EQ(family.response_id, 0x7e8U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_EQ(family.lead_pad_len, 0x8000U);
    EXPECT_EQ(family.tail_pad_len, 0x100U);
}

TEST(SubaruDensoSh72543CanDieselPlan, WriteImageIsBasedAtAddressZero)
{
    // Legacy reflash_block indexed newdata[i + blockctr * blocksize], an image
    // base of 0x8000, while read_memory returned an image based at 0x0 -- so a
    // full ROM was written 0x8000 low. This port bases the write image at 0x0,
    // matching the read output and the three sibling families.
    const auto plan = build_subaru_denso_sh72543_can_diesel_plan(
        FlashOperation::Write, "sub_ecu_denso_sh72543_can_diesel", "SH72543d", bytes::Bytes(0x200000, 0x00));

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->image()->size(), 0x200000U);
    EXPECT_THAT(plan->transfer_region(), RegionIs(MemoryRegion{.start = 0x8000, .length = 0x1F7F00}));
}
} // namespace
} // namespace fastecu::flash::testing
