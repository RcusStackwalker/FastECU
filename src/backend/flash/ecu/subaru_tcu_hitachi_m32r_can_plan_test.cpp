// subaru_tcu_hitachi_m32r_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/testing/single_window_plan_cases.h"

namespace fastecu::flash::testing
{
namespace
{
constexpr SingleWindowPlanCase kCase{
    .name = "SubaruTcuHitachiM32rCan",
    .build = &build_subaru_tcu_hitachi_m32r_can_plan,
    .protocol = "sub_tcu_hitachi_m32r_can",
    .mcu = "M32R_512KB",
    .foreign_protocol = "sub_tcu_hitachi_m32r_can_typo",
    .foreign_mcu = "M32R_512KB_1block",
    // Legacy read_mem computes start_addr - 0x00100000 with start_addr == 0,
    // which underflows uint32_t to 0xFFF00000 and bypasses the "< 0x8000"
    // floor clamp entirely -- see subaru_tcu_hitachi_m32r_can_types.h's
    // divergence 2. This plan targets the clamp's evident intent (0x8000)
    // instead of the never-observed underflowed address.
    .read_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .erase_region = MemoryRegion{.start = 0x8000, .length = 0x78000},
    .image_size = 0x80000,
};

INSTANTIATE_TEST_SUITE_P(SubaruTcuHitachiM32rCan, SingleWindowPlanContract, ::testing::Values(kCase), caseName);

// The wire parameters are this family's own; they do not generalize.
TEST(SubaruTcuHitachiM32rCanPlan, ReadPlanCarriesThisFamilysWireParameters)
{
    const auto plan = build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation::Read, "sub_tcu_hitachi_m32r_can",
                                                             "M32R_512KB", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruTcuHitachiM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7E1U);
    EXPECT_EQ(family.response_id, 0x7E9U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_EQ(family.page_size, 0x100U);
    EXPECT_EQ(family.write_frame_size, 128U);
}

// Deliberate divergence 1, and the safety-critical assertion of this whole
// task: the legacy reflash_block ignores its test_write_arg parameter
// entirely (see subaru_tcu_hitachi_m32r_can_types.h), so "test write" would
// perform the same live erase and flash write as "write" if ported as-is.
// This plan has no dry-run to port and rejects TestWrite outright.
// SingleWindowPlanContract.TestWriteIsRejectedBeforeAnyIo already covers
// this (unconditionally, regardless of supports_write); this test restates
// it directly because it is the requirement the whole task exists to meet.
TEST(SubaruTcuHitachiM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation::TestWrite, "sub_tcu_hitachi_m32r_can",
                                                             "M32R_512KB", bytes::Bytes(0x80000, 0x00));
    EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::Unsupported));
}
} // namespace
} // namespace fastecu::flash::testing
