// subaru_tcu_cvt_mitsu_mh8111_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruTcuCvtMitsuMh8111Can",
    .build = &BuildSubaruTcuCvtMitsuMh8111CanPlan,
    .protocol = "sub_tcu_cvt_mitsu_mh8111_can",
    .mcu = "MH8111",
    .foreign_protocol = "sub_tcu_cvt_mitsu_mh8111_can_typo",
    .foreign_mcu = "MH8104",
    // MH8111's flash geometry is {0,0x40000},{0x40000,0x20000},{0x60000,0x20000},
    // {0x80000,0x100000} (kernelmemorymodels.h kFlashBlocksMH8111); block_modified
    // skips blocks 0-2, so write_mem's only reflash_block call targets block
    // 3: {0x80000, 0x100000}. read_mem hardcodes {0x8000, 0x78000}
    // regardless. These two regions do NOT overlap -- a genuine legacy
    // asymmetry, not a copy/paste error, preserved exactly rather than
    // "fixed" into symmetry (see the non-overlap test below).
    .read_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .erase_region = MemoryRegion{.start = 0x80000, .length = 0x100000},
    .image_size = 0x180000,
};

INSTANTIATE_TEST_SUITE_P(SubaruTcuCvtMitsuMh8111Can, SingleWindowPlanContract, ::testing::Values(kCase), CaseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruTcuCvtMitsuMh8111CanPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan = BuildSubaruTcuCvtMitsuMh8111CanPlan(FlashOperation::kRead, "sub_tcu_cvt_mitsu_mh8111_can",
                                                          "MH8111", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruTcuCvtMitsuMh8111CanPlan>(plan->FamilyPlan());
    EXPECT_EQ(family.request_id, 0x7e1U);
    EXPECT_EQ(family.response_id, 0x7e9U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
}

// Explicit non-overlap assertion: the read window ends exactly where the
// write (erase) window begins. Not reducible to the two individually-stated
// regions above -- it is a relationship between them.
TEST(SubaruTcuCvtMitsuMh8111CanPlan, ReadCoversTheLowerWindowWriteCoversTheUpperBlock)
{
    const auto read_plan = BuildSubaruTcuCvtMitsuMh8111CanPlan(FlashOperation::kRead, "sub_tcu_cvt_mitsu_mh8111_can",
                                                               "MH8111", std::nullopt);
    ASSERT_THAT(read_plan, fastecu::testing::IsOk());

    const auto write_plan = BuildSubaruTcuCvtMitsuMh8111CanPlan(FlashOperation::kWrite, "sub_tcu_cvt_mitsu_mh8111_can",
                                                                "MH8111", bytes::Bytes(0x180000, 0x00));
    ASSERT_THAT(write_plan, fastecu::testing::IsOk());

    EXPECT_EQ(read_plan->TransferRegion().start + read_plan->TransferRegion().length,
              write_plan->TransferRegion().start);
}
} // namespace
} // namespace fastecu::flash::testing
