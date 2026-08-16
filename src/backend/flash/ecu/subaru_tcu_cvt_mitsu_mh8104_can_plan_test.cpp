// subaru_tcu_cvt_mitsu_mh8104_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::build_subaru_tcu_cvt_mitsu_mh8104_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::SubaruTcuCvtMitsuMh8104CanPlan;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "sub_tcu_cvt_mitsu_mh8104_can";
constexpr std::string_view kMcu = "MH8104";

TEST(SubaruTcuCvtMitsuMh8104CanPlan, RejectsUnknownProtocol)
{
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8104_can_plan(
        FlashOperation::Read, "sub_tcu_cvt_mitsu_mh8104_can_typo", kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruTcuCvtMitsuMh8104CanPlan, RejectsMismatchedMcu)
{
    const auto plan =
        build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Read, kProtocol, "MH8111", std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

// MH8104's flash geometry is {0,0x4000},{0x4000,0x2000},{0x6000,0x2000},
// {0x8000,0x78000} (kernelmemorymodels.h fblocks_MH8104); block_modified
// skips blocks 0-2, so write_mem's only reflash_block call targets block 3:
// {0x8000, 0x78000}. read_mem hardcodes the SAME {0x8000, 0x78000} window --
// unlike MH8111, this family's read window and its sole flashed block
// coincide exactly (both are fblocks_MH8104[3]).
TEST(SubaruTcuCvtMitsuMh8104CanPlan, ReadAndWriteShareTheSameWindow)
{
    const auto readPlan = build_subaru_tcu_cvt_mitsu_mh8104_can_plan(
        FlashOperation::Read, "sub_tcu_cvt_mitsu_mh8104_can", "MH8104", std::nullopt);
    ASSERT_TRUE(readPlan.has_value()) << readPlan.error().detail;
    EXPECT_EQ(readPlan->transfer_region().start, 0x8000u);
    EXPECT_EQ(readPlan->transfer_region().length, 0x78000u);

    const auto writePlan = build_subaru_tcu_cvt_mitsu_mh8104_can_plan(
        FlashOperation::Write, "sub_tcu_cvt_mitsu_mh8104_can", "MH8104", bytes::Bytes(0x80000, 0x00));
    ASSERT_TRUE(writePlan.has_value()) << writePlan.error().detail;
    EXPECT_EQ(writePlan->transfer_region().start, 0x8000u);
    EXPECT_EQ(writePlan->transfer_region().length, 0x78000u);
    ASSERT_TRUE(writePlan->image().has_value());
    EXPECT_EQ(writePlan->image()->size(), 0x80000u);

    // Explicit coincidence assertion: unlike MH8111, the two windows are
    // identical, not merely adjacent.
    EXPECT_EQ(readPlan->transfer_region().start, writePlan->transfer_region().start);
    EXPECT_EQ(readPlan->transfer_region().length, writePlan->transfer_region().length);

    const auto& family = std::get<SubaruTcuCvtMitsuMh8104CanPlan>(readPlan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e1u);
    EXPECT_EQ(family.response_id, 0x7e9u);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_TRUE(readPlan->confirmations().empty());
    EXPECT_FALSE(readPlan->kernel().has_value());
}

TEST(SubaruTcuCvtMitsuMh8104CanPlan, WritePlanErasesTheSharedWindow)
{
    const auto plan =
        build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Write, kProtocol, kMcu, bytes::Bytes(0x80000, 0x00));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ASSERT_EQ(plan->erase_regions().size(), 1u);
    EXPECT_EQ(plan->erase_regions()[0].start, 0x8000u);
    EXPECT_EQ(plan->erase_regions()[0].length, 0x78000u);
}

TEST(SubaruTcuCvtMitsuMh8104CanPlan, RejectsAWriteWhoseImageSizeIsWrong)
{
    // The family's true declared capacity is 0x80000 (512*1024, MH8104's
    // romsize) -- reflash_block indexes the encrypted buffer at absolute
    // offsets up to 0x80000 (fblocks[3].start + fblocks[3].len).
    const auto plan =
        build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Write, kProtocol, kMcu, bytes::Bytes(0x78000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, HasSubstr("0x80000"));
}

TEST(SubaruTcuCvtMitsuMh8104CanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::TestWrite, kProtocol, kMcu,
                                                                 bytes::Bytes(0x80000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruTcuCvtMitsuMh8104CanPlan, RejectsAWriteWithNoImage)
{
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}
} // namespace
