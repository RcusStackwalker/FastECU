#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::flash::build_mitsu_colt_m32r_can_plan;
using fastecu::flash::ConfirmationSpec;
using fastecu::flash::FlashOperation;
using fastecu::flash::MitsuColtM32rCanPlan;
using testing::HasSubstr;

constexpr std::string_view kProtocol = "mitsu_ecu_m32r_can";
constexpr std::string_view kMcu = "M32R_384KB_1block";

bytes::Bytes fullRom()
{
    return bytes::Bytes(MitsuColtCan::kTopRegionEnd, 0x00);
}

TEST(MitsuColtM32rCanPlan, ReadPlanCoversTheFirstFlashBlock)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu,
                                                     false, std::nullopt);

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    // Legacy: flashdevices[idx].fblocks[0].{start,len}, transcribed from
    // flash_ecu_mitsu_m32r_can_operation.cpp:44-45.
    EXPECT_EQ(plan->transfer_region().start, 0x00008000u);
    EXPECT_EQ(plan->transfer_region().length, 0x00058000u);
    EXPECT_TRUE(plan->erase_regions().empty());
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_TRUE(plan->confirmations().empty());
}

TEST(MitsuColtM32rCanPlan, ReadPlanSelectsTheBasicDiagnosticSession)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kProtocol, kMcu,
                                                     false, std::nullopt);

    ASSERT_TRUE(plan.has_value());
    const auto& family = std::get<MitsuColtM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.session_id, MitsuColtCan::kSessionBasic);
    EXPECT_FALSE(family.use_vendor_challenge);
    EXPECT_EQ(family.request_id, 0x7e0u);
    EXPECT_EQ(family.response_id, 0x7e8u);
    EXPECT_EQ(family.bitrate, 500000);
    EXPECT_FALSE(family.extended_id);
}

TEST(MitsuColtM32rCanPlan, VendorExtensionProtocolSetsTheChallengeFlag)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(
        FlashOperation::Read, "mitsu_ecu_m32r_can_vendor_ext", kMcu, true, std::nullopt);

    ASSERT_TRUE(plan.has_value());
    EXPECT_TRUE(std::get<MitsuColtM32rCanPlan>(plan->family_plan()).use_vendor_challenge);
}

TEST(MitsuColtM32rCanPlan, WritePlanSelectsBootloadSessionAndDeclaresBothGates)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     false, fullRom());

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(std::get<MitsuColtM32rCanPlan>(plan->family_plan()).session_id,
              MitsuColtCan::kSessionBootload);
    EXPECT_THAT(plan->confirmations(),
                testing::UnorderedElementsAre(
                    testing::Field(&ConfirmationSpec::id, ConfirmationSpec::Id::EraseTrigger),
                    testing::Field(&ConfirmationSpec::id,
                                   ConfirmationSpec::Id::TopRegionBootstrap)));
}

TEST(MitsuColtM32rCanPlan, WritePlanTransfersTheUserspaceRange)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     false, fullRom());

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->transfer_region().start, MitsuColtCan::kUserspaceStart);
    EXPECT_EQ(plan->transfer_region().length,
              MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart);
}

TEST(MitsuColtM32rCanPlan, RejectsAnUnknownMcuType)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kProtocol,
                                                     "NOT_A_REAL_MCU", false, std::nullopt);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:27.
    EXPECT_THAT(plan.error().detail, HasSubstr("Unknown MCU type: NOT_A_REAL_MCU"));
}

TEST(MitsuColtM32rCanPlan, RejectsARomShorterThanTheTopRegionEnd)
{
    bytes::Bytes shortRom(MitsuColtCan::kTopRegionEnd - 1, 0x00);

    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     false, std::move(shortRom));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:400.
    EXPECT_THAT(plan.error().detail, HasSubstr("ROM file too small"));
}

TEST(MitsuColtM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::TestWrite, kProtocol,
                                                     kMcu, false, fullRom());

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
    EXPECT_THAT(plan.error().detail, HasSubstr("test_write"));
}

TEST(MitsuColtM32rCanPlan, RejectsAWriteWithNoImage)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     false, std::nullopt);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

} // namespace
