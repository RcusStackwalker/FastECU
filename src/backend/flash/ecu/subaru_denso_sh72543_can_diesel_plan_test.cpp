// subaru_denso_sh72543_can_diesel_plan_test.cpp
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::build_subaru_denso_sh72543_can_diesel_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::SubaruDensoSh72543CanDieselPlan;
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::string_view kProtocol = "sub_ecu_denso_sh72543_can_diesel";
constexpr std::string_view kMcu = "SH72543d";

TEST(SubaruDensoSh72543CanDieselPlan, ReadPlanCarriesMainFlashBlock)
{
    auto plan = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0x00008000U);
    EXPECT_EQ(plan->transfer_region().length, 0x001F7F00U);
    EXPECT_THAT(plan->erase_regions(), IsEmpty());
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_TRUE(plan->confirmations().empty());

    const auto& family = std::get<SubaruDensoSh72543CanDieselPlan>(plan->family_plan());
    EXPECT_EQ(family.request_id, 0x7e0U);
    EXPECT_EQ(family.response_id, 0x7e8U);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
    EXPECT_EQ(family.lead_pad_len, 0x8000U);
    EXPECT_EQ(family.tail_pad_len, 0x100U);
}

TEST(SubaruDensoSh72543CanDieselPlan, AcceptsSingleBlockGeometry)
{
    // fblocks_SH72543d has numblocks == 1 with fblocks[0] == {0x8000, 0x1F7F00};
    // this family is the only one in the wave with a single-block flash table.
    auto plan = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0x8000U);
    EXPECT_EQ(plan->transfer_region().length, 0x1F7F00U);
}

TEST(SubaruDensoSh72543CanDieselPlan, TestWriteIsRejectedBeforeAnyIo)
{
    auto plan = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::TestWrite, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruDensoSh72543CanDieselPlan, WriteImageIsBasedAtAddressZero)
{
    // Legacy reflash_block indexed newdata[i + blockctr * blocksize], an image
    // base of 0x8000, while read_memory returned an image based at 0x0 -- so a
    // full ROM was written 0x8000 low. This port bases the write image at 0x0,
    // matching the read output and the three sibling families.
    auto plan = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Write, kProtocol, kMcu,
                                                           bytes::Bytes(0x200000, 0x00));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->image()->size(), 0x200000U);
    EXPECT_EQ(plan->transfer_region().start, 0x8000U);
}

TEST(SubaruDensoSh72543CanDieselPlan, WriteRequiresFullStartAlignedImage)
{
    // reflash_block indexes newdata[i + blockctr * blocksize] off a base of
    // fblocks[0].start == 0x0, so the image must span fblocks[0].start..end,
    // i.e. 0x200000 bytes.
    auto tooShort = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Write, kProtocol, kMcu,
                                                               bytes::Bytes(0x1F7F00, 0x00));
    ASSERT_FALSE(tooShort.has_value());
    EXPECT_EQ(tooShort.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(tooShort.error().detail, HasSubstr("0x200000"));

    auto ok = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Write, kProtocol, kMcu,
                                                         bytes::Bytes(0x200000, 0x00));
    ASSERT_TRUE(ok.has_value()) << ok.error().detail;
    ASSERT_EQ(ok->erase_regions().size(), 1U);
    EXPECT_EQ(ok->erase_regions()[0].start, 0x00008000U);
    EXPECT_EQ(ok->erase_regions()[0].length, 0x001F7F00U);
}

TEST(SubaruDensoSh72543CanDieselPlan, WriteWithNoImageIsRejected)
{
    auto plan = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruDensoSh72543CanDieselPlan, WrongProtocolAndWrongMcuAreRejected)
{
    EXPECT_FALSE(build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Read, "sub_ecu_denso_sh72531_can", kMcu,
                                                            std::nullopt)
                     .has_value());
    EXPECT_FALSE(build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Read, kProtocol, "SH72531", std::nullopt)
                     .has_value());
}
} // namespace
