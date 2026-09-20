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
FlashPlanFields fields(FlashOperation op = FlashOperation::Write)
{
    return {.operation = op,
            .family = FlashFamily::SubaruHitachiSh72543rCan,
            .transport = TransportKind::CanIso15765,
            .target_id = kProtocol,
            .mcu_name = "SH72543R",
            .transfer_region = op == FlashOperation::Read ? MemoryRegion{0, 0x200000} : MemoryRegion{0x6000, 0x1FA000},
            .erase_regions =
                op == FlashOperation::Read ? std::vector<MemoryRegion>{} : std::vector{MemoryRegion{0x6000, 0x1FA000}},
            .image = op == FlashOperation::Read ? std::nullopt : std::optional{bytes::Bytes(0x200000, 0xa5)},
            .kernel = std::nullopt,
            .family_plan = SubaruHitachiSh72543rCanPlan{0x7e0, 0x7e8, 500000, false, 0x400, 0x100},
            .confirmations = {}};
}
TEST(Sh72543rPlan, BothAliasesHaveDistinctReadAndWriteWindows)
{
    for (auto protocol : {kProtocol, "sub_ecu_hitachi_sh72543r_can_recovery"})
    {
        auto read = build_subaru_hitachi_sh72543r_can_plan(FlashOperation::Read, protocol, "SH72543R", std::nullopt);
        ASSERT_THAT(read, fastecu::testing::IsOk());
        EXPECT_EQ(read->transfer_region().start, 0U);
        EXPECT_EQ(read->transfer_region().length, 0x200000U);
        EXPECT_TRUE(read->erase_regions().empty());
        EXPECT_FALSE(read->kernel());
        auto write = build_subaru_hitachi_sh72543r_can_plan(FlashOperation::Write, protocol, "SH72543R",
                                                            bytes::Bytes(0x200000, 0xa5));
        ASSERT_THAT(write, fastecu::testing::IsOk());
        EXPECT_EQ(write->transfer_region().start, 0x6000U);
        EXPECT_EQ(write->transfer_region().length, 0x1fa000U);
        ASSERT_EQ(write->erase_regions().size(), 1U);
        EXPECT_EQ(write->erase_regions()[0].start, 0x6000U);
        EXPECT_EQ(write->erase_regions()[0].length, 0x1fa000U);
        EXPECT_EQ(write->image()->size(), 0x200000U);
    }
}
TEST(Sh72543rPlan, RejectsUnsupportedOperationsWithoutImage)
{
    for (auto op : {FlashOperation::TestWrite, static_cast<FlashOperation>(99)})
    {
        EXPECT_THAT(build_subaru_hitachi_sh72543r_can_plan(op, kProtocol, "SH72543R", std::nullopt),
                    fastecu::testing::IsErr(ErrorKind::Unsupported));
        auto built = validate_and_build(fields(op));
        ASSERT_THAT(built, fastecu::testing::IsOk());
        EXPECT_THAT(validate_subaru_hitachi_sh72543r_can_plan(*built), fastecu::testing::IsErr(ErrorKind::Unsupported));
    }
}
TEST(Sh72543rPlan, RejectsIdentityAndImageErrors)
{
    EXPECT_THAT(build_subaru_hitachi_sh72543r_can_plan(
                    FlashOperation::Read, "sub_ecu_hitachi_sh72543r_can_recovery_typo", "SH72543R", std::nullopt),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_hitachi_sh72543r_can_plan(FlashOperation::Read, kProtocol, "SH72543d", std::nullopt),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_hitachi_sh72543r_can_plan(FlashOperation::Read, kProtocol, "SH72543R", bytes::Bytes{}),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_hitachi_sh72543r_can_plan(FlashOperation::Write, kProtocol, "SH72543R", std::nullopt),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    for (auto size : {0U, 0x1fffffU, 0x200001U})
        EXPECT_THAT(
            build_subaru_hitachi_sh72543r_can_plan(FlashOperation::Write, kProtocol, "SH72543R", bytes::Bytes(size)),
            fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}
TEST(Sh72543rPlan, ForgedPlansCannotChangeWireOrGeometry)
{
    for (int mutation = 0; mutation < 16; ++mutation)
    {
        SCOPED_TRACE(mutation);
        auto f = fields();
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
            f.image->pop_back();
            break;
        case 12:
            f.image->push_back(0);
            break;
        case 13:
            f.target_id = "wrong";
            break;
        case 14:
            f.mcu_name = "SH72543d";
            break;
        case 15:
            f.confirmations.push_back({ConfirmationSpec::Id::EraseTrigger, {}});
            break;
        }
        auto built = validate_and_build(std::move(f));
        ASSERT_THAT(built, fastecu::testing::IsOk());
        EXPECT_THAT(validate_subaru_hitachi_sh72543r_can_plan(*built),
                    fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    }
}
TEST(Sh72543rPlan, RejectsKernelAndReadWindowDrift)
{
    auto f = fields();
    f.kernel = KernelImage{"unused", 0, bytes::Bytes{1}};
    auto built = validate_and_build(std::move(f));
    ASSERT_THAT(built, fastecu::testing::IsOk());
    EXPECT_THAT(validate_subaru_hitachi_sh72543r_can_plan(*built), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    f = fields(FlashOperation::Read);
    f.transfer_region = {0x6000, 0x1fa000};
    built = validate_and_build(std::move(f));
    ASSERT_THAT(built, fastecu::testing::IsOk());
    EXPECT_THAT(validate_subaru_hitachi_sh72543r_can_plan(*built), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}
TEST(Sh72543rPlan, CoreRejectsMismatchedFamilyTransportAndVariant)
{
    for (int mutation = 0; mutation < 3; ++mutation)
    {
        auto f = fields();
        if (mutation == 0)
            f.family = FlashFamily::SubaruTcuHitachiM32rCan;
        if (mutation == 1)
            f.transport = TransportKind::Kline;
        if (mutation == 2)
            f.family_plan = SubaruTcuHitachiM32rCanPlan{};
        EXPECT_THAT(validate_and_build(std::move(f)), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    }
}
} // namespace
} // namespace fastecu::flash
