// subaru_tcu_cvt_mitsu_mh8111_can_plan_test.cpp
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::build_subaru_tcu_cvt_mitsu_mh8111_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::SubaruTcuCvtMitsuMh8111CanPlan;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "sub_tcu_cvt_mitsu_mh8111_can";
constexpr std::string_view kMcu = "MH8111";

TEST(SubaruTcuCvtMitsuMh8111CanPlan, RejectsUnknownProtocol)
{
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(
        FlashOperation::Read, "sub_tcu_cvt_mitsu_mh8111_can_typo", kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruTcuCvtMitsuMh8111CanPlan, RejectsMismatchedMcu)
{
    const auto plan =
        build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::Read, kProtocol, "MH8104", std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

// MH8111's flash geometry is {0,0x40000},{0x40000,0x20000},{0x60000,0x20000},
// {0x80000,0x100000} (kernelmemorymodels.h fblocks_MH8111); block_modified
// skips blocks 0-2, so write_mem's only reflash_block call targets block 3:
// {0x80000, 0x100000}. read_mem hardcodes {0x8000, 0x78000} regardless.
// These two regions do NOT overlap -- a genuine legacy asymmetry, not a
// copy/paste error, preserved exactly rather than "fixed" into symmetry.
TEST(SubaruTcuCvtMitsuMh8111CanPlan, ReadCoversTheLowerWindowWriteCoversTheUpperBlock)
{
    const auto readPlan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(
        FlashOperation::Read, "sub_tcu_cvt_mitsu_mh8111_can", "MH8111", std::nullopt);
    ASSERT_TRUE(readPlan.has_value()) << readPlan.error().detail;
    EXPECT_EQ(readPlan->transfer_region().start, 0x8000U);
    EXPECT_EQ(readPlan->transfer_region().length, 0x78000U);

    const auto writePlan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(
        FlashOperation::Write, "sub_tcu_cvt_mitsu_mh8111_can", "MH8111", bytes::Bytes(0x180000, 0x00));
    ASSERT_TRUE(writePlan.has_value()) << writePlan.error().detail;
    EXPECT_EQ(writePlan->transfer_region().start, 0x80000U);
    EXPECT_EQ(writePlan->transfer_region().length, 0x100000U);
    ASSERT_TRUE(writePlan->image().has_value());
    EXPECT_EQ(writePlan->image()->size(), 0x180000U);

    // Explicit non-overlap assertion: the read window ends exactly where the
    // write window begins.
    EXPECT_EQ(readPlan->transfer_region().start + readPlan->transfer_region().length,
              writePlan->transfer_region().start);

    const auto& family = std::get<SubaruTcuCvtMitsuMh8111CanPlan>(readPlan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e1U);
    EXPECT_EQ(family.response_id, 0x7e9U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_TRUE(readPlan->confirmations().empty());
    EXPECT_FALSE(readPlan->kernel().has_value());
}

TEST(SubaruTcuCvtMitsuMh8111CanPlan, WritePlanErasesOnlyTheUpperBlock)
{
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                                 bytes::Bytes(0x180000, 0x00));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ASSERT_EQ(plan->erase_regions().size(), 1U);
    EXPECT_EQ(plan->erase_regions()[0].start, 0x80000U);
    EXPECT_EQ(plan->erase_regions()[0].length, 0x100000U);
}

TEST(SubaruTcuCvtMitsuMh8111CanPlan, RejectsAWriteWhoseImageSizeIsWrong)
{
    // The family's true declared capacity is 0x180000 (3*512*1024), not the
    // write window's own length (0x100000) -- reflash_block indexes the
    // encrypted buffer at absolute offsets up to 0x180000.
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                                 bytes::Bytes(0x100000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, HasSubstr("0x180000"));
}

TEST(SubaruTcuCvtMitsuMh8111CanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::TestWrite, kProtocol, kMcu,
                                                                 bytes::Bytes(0x180000, 0x00));
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruTcuCvtMitsuMh8111CanPlan, RejectsAWriteWithNoImage)
{
    const auto plan = build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}
} // namespace
