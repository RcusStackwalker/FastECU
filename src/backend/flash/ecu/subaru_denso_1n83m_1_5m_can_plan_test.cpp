// subaru_denso_1n83m_1_5m_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::build_subaru_denso_1n83m_1_5m_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::SubaruDenso1n83m_1_5mCanPlan;
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_1_5m_can";
constexpr std::string_view kMcu = "N83M_1_5MB";

TEST(SubaruDenso1n83m_1_5mCanPlan, ReadPlanCarriesMainFlashBlock)
{
    auto plan = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0x08FAC000u);
    EXPECT_EQ(plan->transfer_region().length, 0x00173F00u);
    EXPECT_THAT(plan->erase_regions(), IsEmpty());
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_TRUE(plan->confirmations().empty());

    const auto& family = std::get<SubaruDenso1n83m_1_5mCanPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e0u);
    EXPECT_EQ(family.response_id, 0x7e8u);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_EQ(family.lead_pad_len, 0x10000u);
    EXPECT_EQ(family.tail_pad_len, 0x100u);
}

TEST(SubaruDenso1n83m_1_5mCanPlan, TestWriteIsRejectedBeforeAnyIo)
{
    auto plan = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::TestWrite, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruDenso1n83m_1_5mCanPlan, WriteRequiresFullStartAlignedImage)
{
    // reflash_block indexes newdata[i + blockaddr - fblocks[0].start], so the
    // image must span fblocks[0].start..end, i.e. 0x184000 bytes.
    auto tooShort =
        build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Write, kProtocol, kMcu, bytes::Bytes(0x173F00, 0x00));
    ASSERT_FALSE(tooShort.has_value());
    EXPECT_EQ(tooShort.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(tooShort.error().detail, HasSubstr("0x184000"));

    auto ok =
        build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Write, kProtocol, kMcu, bytes::Bytes(0x184000, 0x00));
    ASSERT_TRUE(ok.has_value()) << ok.error().detail;
    ASSERT_EQ(ok->erase_regions().size(), 1u);
    EXPECT_EQ(ok->erase_regions()[0].start, 0x08FAC000u);
    EXPECT_EQ(ok->erase_regions()[0].length, 0x00173F00u);
}

TEST(SubaruDenso1n83m_1_5mCanPlan, WriteWithNoImageIsRejected)
{
    auto plan = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruDenso1n83m_1_5mCanPlan, WrongProtocolAndWrongMcuAreRejected)
{
    EXPECT_FALSE(
        build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Read, "sub_ecu_denso_sh72531_can", kMcu, std::nullopt)
            .has_value());
    EXPECT_FALSE(
        build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Read, kProtocol, "SH72531", std::nullopt).has_value());
}
} // namespace
