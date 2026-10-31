#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_types.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_types.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/testing/result_matchers.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <array>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

using ::testing::Eq;
using ::testing::IsEmpty;

namespace fastecu::flash
{
namespace
{
constexpr auto kProtocol = "sub_ecu_hitachi_sh72543r_can";
FlashPlanFields Fields(FlashOperation op = FlashOperation::kWrite)
{
    return {.operation = op,
            .target_id = kProtocol,
            .mcu_name = "SH72543R",
            .transfer_region = op == FlashOperation::kRead ? MemoryRegion{0, 0x200000} : MemoryRegion{0x6000, 0x1FA000},
            .erase_regions =
                op == FlashOperation::kRead ? std::vector<MemoryRegion>{} : std::vector{MemoryRegion{0x6000, 0x1FA000}},
            .image = op == FlashOperation::kRead ? std::nullopt : std::optional{bytes::Bytes(0x200000, 0xa5)},
            .kernel = std::nullopt,
            .family_plan = SubaruHitachiSh72543rCanPlan{0x7e0, 0x7e8, 500000, false, 0x400, 0x100},
            .confirmations = {}};
}
TEST(Sh72543rPlan, BothAliasesHaveDistinctReadAndWriteWindows)
{
    for (auto protocol : {kProtocol, "sub_ecu_hitachi_sh72543r_can_recovery"})
    {
        SCOPED_TRACE(::testing::PrintToString(protocol));
        auto read = BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kRead, protocol, "SH72543R", std::nullopt);
        ASSERT_THAT(read, fastecu::testing::IsOk());
        EXPECT_EQ(read->TransferRegion().start, 0U);
        EXPECT_EQ(read->TransferRegion().length, 0x200000U);
        EXPECT_THAT(read->EraseRegions(), IsEmpty());
        EXPECT_THAT(read->Kernel(), Eq(std::nullopt));
        auto write = BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kWrite, protocol, "SH72543R",
                                                       bytes::Bytes(0x200000, 0xa5));
        ASSERT_THAT(write, fastecu::testing::IsOk());
        EXPECT_EQ(write->TransferRegion().start, 0x6000U);
        EXPECT_EQ(write->TransferRegion().length, 0x1fa000U);
        ASSERT_EQ(write->EraseRegions().size(), 1U);
        EXPECT_EQ(write->EraseRegions()[0].start, 0x6000U);
        EXPECT_EQ(write->EraseRegions()[0].length, 0x1fa000U);
        EXPECT_EQ(write->ImageOrEmpty().size(), 0x200000U);
    }
}
TEST(Sh72543rPlan, RejectsUnsupportedOperationsWithoutImage)
{
    constexpr auto kOp = FlashOperation::kTestWrite;
    EXPECT_THAT(BuildSubaruHitachiSh72543rCanPlan(kOp, kProtocol, "SH72543R", std::nullopt),
                fastecu::testing::IsErr(ErrorKind::kUnsupported));
    auto built = ValidateAndBuild(Fields(kOp));
    ASSERT_THAT(built, fastecu::testing::IsOk());
    EXPECT_THAT(ValidateSubaruHitachiSh72543rCanPlan(*built), fastecu::testing::IsErr(ErrorKind::kUnsupported));
}
TEST(Sh72543rPlan, RejectsIdentityAndImageErrors)
{
    EXPECT_THAT(BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kRead, "sub_ecu_hitachi_sh72543r_can_recovery_typo",
                                                  "SH72543R", std::nullopt),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kRead, kProtocol, "SH72543d", std::nullopt),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kRead, kProtocol, "SH72543R", bytes::Bytes{}),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kWrite, kProtocol, "SH72543R", std::nullopt),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    for (auto size : {0U, 0x1fffffU, 0x200001U})
    {
        SCOPED_TRACE(::testing::PrintToString(size));
        EXPECT_THAT(
            BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kWrite, kProtocol, "SH72543R", bytes::Bytes(size)),
            fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}
SubaruHitachiSh72543rCanPlan& WireOf(FlashPlanFields& f)
{
    return f.family_plan.Get<SubaruHitachiSh72543rCanPlan>();
}
TEST(Sh72543rPlan, ForgedPlansCannotChangeWireOrGeometry)
{
    const std::vector<std::pair<std::string_view, std::function<void(FlashPlanFields&)>>> cases{
        {"request id", [](FlashPlanFields& f) { WireOf(f).request_id++; }},
        {"response id", [](FlashPlanFields& f) { WireOf(f).response_id++; }},
        {"bitrate", [](FlashPlanFields& f) { WireOf(f).bitrate = 250000; }},
        {"extended id", [](FlashPlanFields& f) { WireOf(f).extended_id = true; }},
        {"page size", [](FlashPlanFields& f) { WireOf(f).page_size = 0x100; }},
        {"write frame size", [](FlashPlanFields& f) { WireOf(f).write_frame_size = 128; }},
        {"transfer start", [](FlashPlanFields& f) { f.transfer_region.start = 0; }},
        {"transfer length", [](FlashPlanFields& f) { f.transfer_region.length--; }},
        {"no erase region", [](FlashPlanFields& f) { f.erase_regions.clear(); }},
        {"erase start", [](FlashPlanFields& f) { f.erase_regions[0].start = 0; }},
        {"erase length", [](FlashPlanFields& f) { f.erase_regions[0].length--; }},
        {"short image", [](FlashPlanFields& f) { f.image = bytes::Bytes(0x1fffff, 0xa5); }},
        {"long image", [](FlashPlanFields& f) { f.image = bytes::Bytes(0x200001, 0xa5); }},
        {"protocol", [](FlashPlanFields& f) { f.target_id = "wrong"; }},
        {"MCU", [](FlashPlanFields& f) { f.mcu_name = "SH72543d"; }},
        {"confirmation",
         [](FlashPlanFields& f) { f.confirmations.push_back({ConfirmationSpec::Id::kEraseTrigger, {}}); }},
    };
    for (const auto& [name, mutate] : cases)
    {
        SCOPED_TRACE(name);
        auto f = Fields();
        mutate(f);
        auto built = ValidateAndBuild(std::move(f));
        ASSERT_THAT(built, fastecu::testing::IsOk());
        EXPECT_THAT(ValidateSubaruHitachiSh72543rCanPlan(*built), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}
TEST(Sh72543rPlan, RejectsKernelAndReadWindowDrift)
{
    auto f = Fields();
    f.kernel = KernelImage{"unused", 0, bytes::Bytes{1}};
    auto built = ValidateAndBuild(std::move(f));
    ASSERT_THAT(built, fastecu::testing::IsOk());
    EXPECT_THAT(ValidateSubaruHitachiSh72543rCanPlan(*built), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    f = Fields(FlashOperation::kRead);
    f.transfer_region = {0x6000, 0x1fa000};
    built = ValidateAndBuild(std::move(f));
    ASSERT_THAT(built, fastecu::testing::IsOk());
    EXPECT_THAT(ValidateSubaruHitachiSh72543rCanPlan(*built), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}
// The plan's family comes from its plan type (ADR 0030), so a plan carrying
// another family's plan type builds as that family's plan; this family's
// validator is what turns it away.
TEST(Sh72543rPlan, ValidatorRejectsAnotherFamilysPlanType)
{
    auto f = Fields();
    f.family_plan = SubaruTcuHitachiM32rCanPlan{};
    auto built = ValidateAndBuild(std::move(f));
    ASSERT_THAT(built, fastecu::testing::IsOk());
    EXPECT_EQ(built->Family(), FlashFamily::kSubaruTcuHitachiM32rCan);
    EXPECT_THAT(ValidateSubaruHitachiSh72543rCanPlan(*built), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}
} // namespace
} // namespace fastecu::flash
