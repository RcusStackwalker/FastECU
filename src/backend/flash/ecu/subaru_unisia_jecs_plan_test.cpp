#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

FlashPlanFields fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruUnisiaJecs,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_unisia_jecs_m3779x",
        .mcu_name = "M3779x",
        .transfer_region = MemoryRegion{0, 0x10000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruUnisiaJecsPlan{.initial_baud = 1953, .even_parity = true},
        .confirmations = {},
    };
}

TEST(SubaruUnisiaJecsPlan, MapsBothConfiguredProtocolMcuPairs)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
             {"sub_ecu_unisia_jecs_m3775x", "M3775x"},
         }))
    {
        const auto plan = build_subaru_unisia_jecs_plan(FlashOperation::Read, protocol, mcu, std::nullopt);
        ASSERT_THAT(plan, IsOk());
        EXPECT_EQ(plan->family(), FlashFamily::SubaruUnisiaJecs);
        EXPECT_EQ(plan->transport(), TransportKind::Kline);
        EXPECT_EQ(plan->transfer_region().start, 0U);
        EXPECT_EQ(plan->transfer_region().length, 0x10000U);
        const auto& family = std::get<SubaruUnisiaJecsPlan>(plan->family_plan());
        EXPECT_EQ(family.initial_baud, 1953);
        EXPECT_TRUE(family.even_parity);
    }
}

TEST(SubaruUnisiaJecsPlan, RejectsCrossPairedProtocolAndMcu)
{
    EXPECT_THAT(
        build_subaru_unisia_jecs_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_m3779x", "M3775x", std::nullopt),
        IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(
        build_subaru_unisia_jecs_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_m3775x", "M3779x", std::nullopt),
        IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruUnisiaJecsPlan, RejectsWriteOperations)
{
    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        EXPECT_THAT(
            build_subaru_unisia_jecs_plan(operation, "sub_ecu_unisia_jecs_m3779x", "M3779x", bytes::Bytes(0x10000, 0)),
            IsErr(ErrorKind::Unsupported));
    }
}

TEST(SubaruUnisiaJecsPlan, StandaloneValidatorRejectsForgedFields)
{
    for (int mutation = 0; mutation < 8; ++mutation)
    {
        SCOPED_TRACE(mutation);
        auto forged = fields();
        auto& wire = std::get<SubaruUnisiaJecsPlan>(forged.family_plan);
        switch (mutation)
        {
        case 0:
            forged.target_id = "sub_ecu_unisia_jecs_m3775x";
            break;
        case 1:
            forged.mcu_name = "M3775x";
            break;
        case 2:
            forged.transfer_region.start = 1;
            break;
        case 3:
            forged.transfer_region.length = 0x20;
            break;
        case 4:
            wire.initial_baud = 1952;
            break;
        case 5:
            wire.even_parity = false;
            break;
        case 6:
            forged.kernel = KernelImage{"unexpected", 0, {1}};
            break;
        case 7:
            forged.confirmations.push_back({ConfirmationSpec::Id::CycleIgnition, {}});
            break;
        default:
            FAIL() << "unexpected mutation";
        }
        const auto plan = validate_and_build(std::move(forged));
        ASSERT_THAT(plan, IsOk());
        EXPECT_THAT(validate_subaru_unisia_jecs_plan(*plan), IsErr(ErrorKind::InvalidConfig));
    }
}

TEST(SubaruUnisiaJecsPlan, StandaloneValidatorRejectsTestWrite)
{
    auto forged = fields();
    forged.operation = FlashOperation::TestWrite;
    forged.image = bytes::Bytes(0x10000, 0);
    const auto plan = validate_and_build(std::move(forged));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(validate_subaru_unisia_jecs_plan(*plan), IsErr(ErrorKind::Unsupported));
}
} // namespace
} // namespace fastecu::flash
