#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

struct Variant
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
};

constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
    {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
});

bool carries_only_voltage_confirmation(const FlashPlan& plan)
{
    return plan.confirmations().size() == 1 &&
           plan.confirmations()[0].id == ConfirmationSpec::Id::ApplyBootModeVoltages;
}

TEST(SubaruUnisiaJecsM32rBootModePlan, KernelPlanPadsToWholeChunks)
{
    for (const Variant& variant : kVariants)
    {
        const auto plan = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation::Write, variant.protocol,
                                                                             variant.mcu, bytes::Bytes(200, 0x5a));
        ASSERT_THAT(plan, IsOk()) << variant.protocol;
        bytes::Bytes expected(200, 0x5a);
        expected.resize(256, 0x00); // upload_kernel() :312-315
        EXPECT_EQ(plan->image(), std::optional<bytes::Bytes>(expected));
        EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, 256}));
        EXPECT_EQ(plan->family(), FlashFamily::SubaruUnisiaJecsM32rBootModeKernel);
        EXPECT_FALSE(plan->kernel().has_value());
        EXPECT_TRUE(carries_only_voltage_confirmation(*plan));
        EXPECT_THAT(validate_subaru_unisia_jecs_m32r_bootmode_plan(*plan), IsOk());
    }
}

TEST(SubaruUnisiaJecsM32rBootModePlan, KernelAlreadyAlignedIsNotPadded)
{
    const auto plan = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(
        FlashOperation::Write, "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", bytes::Bytes(0x100, 0x11));
    ASSERT_THAT(plan, IsOk());
    EXPECT_EQ(plan->image()->size(), 0x100U);
}

TEST(SubaruUnisiaJecsM32rBootModePlan, EmptyKernelIsRejected)
{
    EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation::Write,
                                                                   "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", {}),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruUnisiaJecsM32rBootModePlan, ProgramPlanTakesExactlyTheRomSize)
{
    for (const Variant& variant : kVariants)
    {
        const auto plan = build_subaru_unisia_jecs_m32r_bootmode_program_plan(
            FlashOperation::Write, variant.protocol, variant.mcu, bytes::Bytes(variant.rom_size, 0x5a));
        ASSERT_THAT(plan, IsOk()) << variant.protocol;
        EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, variant.rom_size}));
        EXPECT_EQ(plan->family(), FlashFamily::SubaruUnisiaJecsM32rBootModeProgram);
        EXPECT_TRUE(carries_only_voltage_confirmation(*plan));
        EXPECT_THAT(validate_subaru_unisia_jecs_m32r_bootmode_plan(*plan), IsOk());

        for (const std::uint32_t size : {variant.rom_size - 1, variant.rom_size + 1, variant.rom_size - 0x40})
        {
            EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation::Write, variant.protocol,
                                                                            variant.mcu, bytes::Bytes(size, 0x5a)),
                        IsErr(ErrorKind::InvalidConfig))
                << variant.protocol << " size " << size;
        }
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation::Write, variant.protocol,
                                                                        variant.mcu, std::nullopt),
                    IsErr(ErrorKind::InvalidConfig));
    }
}

TEST(SubaruUnisiaJecsM32rBootModePlan, RejectsReadAndTestWrite)
{
    for (const FlashOperation operation : {FlashOperation::Read, FlashOperation::TestWrite})
    {
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(operation, "sub_ecu_unisia_jecs_20_bootmode",
                                                                       "M32R_128KB", bytes::Bytes(1, 0x00)),
                    IsErr(ErrorKind::Unsupported));
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_program_plan(operation, "sub_ecu_unisia_jecs_20_bootmode",
                                                                        "M32R_128KB", bytes::Bytes(0x20000, 0x00)),
                    IsErr(ErrorKind::Unsupported));
    }
}

TEST(SubaruUnisiaJecsM32rBootModePlan, RejectsEveryOtherIdentity)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_unisia_jecs_20_bootmode", "M32R_256KB"},
             {"sub_ecu_unisia_jecs_20", "M32R_128KB"},
             {"sub_ecu_unisia_jecs_20_bootmodex", "M32R_128KB"},
             {"sub_ecu_unisia_jecs_40_bootmode", "M32R_384KB"},
         }))
    {
        EXPECT_THAT(build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation::Write, protocol, mcu,
                                                                       bytes::Bytes(1, 0x00)),
                    IsErr(ErrorKind::InvalidConfig))
            << protocol << " / " << mcu;
    }
}

// Presence means granted: a plan assembled without the confirmation (never
// through the builders) must not validate, so no executor raises the lines.
TEST(SubaruUnisiaJecsM32rBootModePlan, ValidatorRejectsAPlanWithoutTheVoltageConfirmation)
{
    auto plan = validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = FlashFamily::SubaruUnisiaJecsM32rBootModeKernel,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_unisia_jecs_20_bootmode",
        .mcu_name = "M32R_128KB",
        .transfer_region = {0, 0x80},
        .erase_regions = {},
        .image = bytes::Bytes(0x80, 0x00),
        .kernel = std::nullopt,
        .family_plan =
            SubaruUnisiaJecsM32rBootModeKernelPlan{.initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10},
        .confirmations = {},
    });
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(validate_subaru_unisia_jecs_m32r_bootmode_plan(*plan), IsErr(ErrorKind::InvalidConfig));
}
} // namespace
} // namespace fastecu::flash
