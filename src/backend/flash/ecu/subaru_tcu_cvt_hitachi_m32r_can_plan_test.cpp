// subaru_tcu_cvt_hitachi_m32r_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::build_subaru_tcu_cvt_hitachi_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::SubaruTcuCvtHitachiM32rCanPlan;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "sub_tcu_cvt_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB";

TEST(SubaruTcuCvtHitachiM32rCanPlan, RejectsUnknownProtocol)
{
    const auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(
        FlashOperation::Read, "sub_tcu_cvt_hitachi_m32r_can_typo", kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruTcuCvtHitachiM32rCanPlan, RejectsMismatchedMcu)
{
    const auto plan =
        build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, "MH8104", std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruTcuCvtHitachiM32rCanPlan, ReadPlanCoversTheClampedWindow)
{
    const auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0x8000U);
    EXPECT_EQ(plan->transfer_region().length, 0x78000U);
    const auto& family = std::get<SubaruTcuCvtHitachiM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e1U);
    EXPECT_EQ(family.response_id, 0x7e9U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_TRUE(plan->confirmations().empty());
    EXPECT_FALSE(plan->kernel().has_value());
}

TEST(SubaruTcuCvtHitachiM32rCanPlan, WritePlanCoversTheClampedWindowAndErasesIt)
{
    const auto plan =
        build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, bytes::Bytes(0x80000, 0x00));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0x8000U);
    EXPECT_EQ(plan->transfer_region().length, 0x78000U);
    ASSERT_EQ(plan->erase_regions().size(), 1U);
    EXPECT_EQ(plan->erase_regions()[0].start, 0x8000U);
    EXPECT_EQ(plan->erase_regions()[0].length, 0x78000U);
}

TEST(SubaruTcuCvtHitachiM32rCanPlan, RejectsAWriteWhoseImageSizeIsWrong)
{
    const auto plan =
        build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, bytes::Bytes(0x60000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, HasSubstr("0x80000"));
}

TEST(SubaruTcuCvtHitachiM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::TestWrite, kProtocol, kMcu,
                                                                 bytes::Bytes(0x80000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruTcuCvtHitachiM32rCanPlan, RejectsAWriteWithNoImage)
{
    const auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruTcuCvtHitachiM32rCanPlan, ReadRegionIsTheFloorClampedWindowNotTheLiteralUnderflow)
{
    // Legacy read_mem computes start_addr - 0x00100000 with start_addr == 0,
    // which underflows uint32_t to 0xFFF00000 and bypasses the
    // "< 0x8000" floor clamp entirely -- this path never executed in
    // production (execute() called hack_words(), never read_mem()). This
    // plan targets the clamp's evident intent (0x8000) rather than
    // reproducing an address computation nothing ever observed on the wire.
    const auto plan = build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation::Read, "sub_tcu_cvt_hitachi_m32r_can",
                                                                 "M32R_512KB", std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0x8000U);
    EXPECT_EQ(plan->transfer_region().length, 0x78000U);
}
} // namespace
