#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>
#include <array>
#include <functional>

namespace fastecu::flash
{
namespace
{
constexpr auto kProtocol = "sub_ecu_hitachi_sh72543r_can";
FlashPlanFields Fields(FlashOperation op = FlashOperation::kWrite)
{
    return {.operation = op,
            .family = FlashFamily::kSubaruHitachiSh72543rCan,
            .transport = TransportKind::kCanIso15765,
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
        auto read = BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kRead, protocol, "SH72543R", std::nullopt);
        ASSERT_THAT(read, fastecu::testing::IsOk());
        EXPECT_EQ(read->TransferRegion().start, 0U);
        EXPECT_EQ(read->TransferRegion().length, 0x200000U);
        EXPECT_TRUE(read->EraseRegions().empty());
        EXPECT_FALSE(read->Kernel());
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
        EXPECT_THAT(
            BuildSubaruHitachiSh72543rCanPlan(FlashOperation::kWrite, kProtocol, "SH72543R", bytes::Bytes(size)),
            fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}
TEST(Sh72543rPlan, ForgedPlansCannotChangeWireOrGeometry)
{
    for (int mutation = 0; mutation < 16; ++mutation)
    {
        SCOPED_TRACE(mutation);
        auto f = Fields();
        auto& wire = std::get<SubaruHitachiSh72543rCanPlan>(f.family_plan);
        switch (mutation)
        {
        case 0:
            wire.request_id++;
            break;
        case 1:
            wire.response_id++;
            break;
        case 2:
            wire.bitrate = 250000;
            break;
        case 3:
            wire.extended_id = true;
            break;
        case 4:
            wire.page_size = 0x100;
            break;
        case 5:
            wire.write_frame_size = 128;
            break;
        case 6:
            f.transfer_region.start = 0;
            break;
        case 7:
            f.transfer_region.length--;
            break;
        case 8:
            f.erase_regions.clear();
            break;
        case 9:
            f.erase_regions[0].start = 0;
            break;
        case 10:
            f.erase_regions[0].length--;
            break;
        case 11:
            ASSERT_TRUE(f.image.has_value());
            f.image->pop_back();
            break;
        case 12:
            ASSERT_TRUE(f.image.has_value());
            f.image->push_back(0);
            break;
        case 13:
            f.target_id = "wrong";
            break;
        case 14:
            f.mcu_name = "SH72543d";
            break;
        case 15:
            f.confirmations.push_back({ConfirmationSpec::Id::kEraseTrigger, {}});
            break;
        default:
            FAIL() << "unexpected mutation";
            break;
        }
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
TEST(Sh72543rPlan, CoreRejectsMismatchedFamilyTransportAndVariant)
{
    for (int mutation = 0; mutation < 3; ++mutation)
    {
        auto f = Fields();
        if (mutation == 0)
        {
            f.family = FlashFamily::kSubaruTcuHitachiM32rCan;
        }
        if (mutation == 1)
        {
            f.transport = TransportKind::kKline;
        }
        if (mutation == 2)
        {
            f.family_plan = SubaruTcuHitachiM32rCanPlan{};
        }
        EXPECT_THAT(ValidateAndBuild(std::move(f)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}
} // namespace
} // namespace fastecu::flash
