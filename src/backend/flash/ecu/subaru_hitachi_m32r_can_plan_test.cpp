// subaru_hitachi_m32r_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::build_subaru_hitachi_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::SubaruHitachiM32rCanPlan;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "sub_ecu_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB_1block";

TEST(SubaruHitachiM32rCanPlan, RejectsUnknownProtocol)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Read,
                                                         "sub_ecu_hitachi_m32r_can_typo", kMcu,
                                                         std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruHitachiM32rCanPlan, RejectsMismatchedMcu)
{
    const auto plan =
        build_subaru_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, "MH8104", std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruHitachiM32rCanPlan, ReadPlanCoversTheFullRomFromZero)
{
    const auto plan =
        build_subaru_hitachi_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0u);
    EXPECT_EQ(plan->transfer_region().length, 0x80000u);
    const auto& family = std::get<SubaruHitachiM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e0u);
    EXPECT_EQ(family.response_id, 0x7e8u);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_TRUE(plan->confirmations().empty());
    EXPECT_FALSE(plan->kernel().has_value());
}

TEST(SubaruHitachiM32rCanPlan, WritePlanCoversTheFullRomAndErasesItAll)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                         bytes::Bytes(0x80000, 0x00));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0u);
    EXPECT_EQ(plan->transfer_region().length, 0x80000u);
    ASSERT_EQ(plan->erase_regions().size(), 1u);
    EXPECT_EQ(plan->erase_regions()[0].start, 0u);
    EXPECT_EQ(plan->erase_regions()[0].length, 0x80000u);
}

TEST(SubaruHitachiM32rCanPlan, RejectsAWriteWhoseImageSizeIsWrong)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                         bytes::Bytes(0x60000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, HasSubstr("0x80000"));
}

TEST(SubaruHitachiM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_subaru_hitachi_m32r_can_plan(FlashOperation::TestWrite, kProtocol,
                                                         kMcu, bytes::Bytes(0x80000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruHitachiM32rCanPlan, RejectsAWriteWithNoImage)
{
    const auto plan =
        build_subaru_hitachi_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}
} // namespace
