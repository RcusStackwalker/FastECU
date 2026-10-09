#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
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

bool CarriesOnlyVoltageConfirmation(const FlashPlan& plan)
{
    return plan.Confirmations().size() == 1 &&
           plan.Confirmations()[0].id == ConfirmationSpec::Id::kApplyBootModeVoltages;
}

TEST(SubaruUnisiaJecsM32rBootModePlan, KernelPlanPadsToWholeChunks)
{
    for (const Variant& variant : kVariants)
    {
        const auto plan = BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(FlashOperation::kWrite, variant.protocol,
                                                                      variant.mcu, bytes::Bytes(200, 0x5a));
        ASSERT_THAT(plan, IsOk()) << variant.protocol;
        bytes::Bytes expected(256, 0x00); // upload_kernel() :312-315 pads to 256
        std::fill_n(expected.begin(), 200, bytes::Byte{0x5a});
        EXPECT_EQ(plan->Image(), std::optional<bytes::Bytes>(expected));
        EXPECT_EQ(plan->TransferRegion(), (MemoryRegion{0, 256}));
        EXPECT_EQ(plan->Family(), FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel);
        EXPECT_FALSE(plan->Kernel().has_value());
        EXPECT_TRUE(CarriesOnlyVoltageConfirmation(*plan));
        EXPECT_THAT(ValidateSubaruUnisiaJecsM32rBootmodePlan(*plan), IsOk());
    }
}

TEST(SubaruUnisiaJecsM32rBootModePlan, KernelAlreadyAlignedIsNotPadded)
{
    const auto plan = BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(
        FlashOperation::kWrite, "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", bytes::Bytes(0x100, 0x11));
    ASSERT_THAT(plan, IsOk());
    EXPECT_EQ(plan->ImageOrEmpty().size(), 0x100U);
}

TEST(SubaruUnisiaJecsM32rBootModePlan, EmptyKernelIsRejected)
{
    EXPECT_THAT(BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(FlashOperation::kWrite, "sub_ecu_unisia_jecs_20_bootmode",
                                                            "M32R_128KB", {}),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruUnisiaJecsM32rBootModePlan, ProgramPlanTakesExactlyTheRomSize)
{
    for (const Variant& variant : kVariants)
    {
        const auto plan = BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(
            FlashOperation::kWrite, variant.protocol, variant.mcu, bytes::Bytes(variant.rom_size, 0x5a));
        ASSERT_THAT(plan, IsOk()) << variant.protocol;
        EXPECT_EQ(plan->TransferRegion(), (MemoryRegion{0, variant.rom_size}));
        EXPECT_EQ(plan->Family(), FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram);
        EXPECT_TRUE(CarriesOnlyVoltageConfirmation(*plan));
        EXPECT_THAT(ValidateSubaruUnisiaJecsM32rBootmodePlan(*plan), IsOk());

        for (const std::uint32_t size : {variant.rom_size - 1, variant.rom_size + 1, variant.rom_size - 0x40})
        {
            EXPECT_THAT(BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(FlashOperation::kWrite, variant.protocol,
                                                                     variant.mcu, bytes::Bytes(size, 0x5a)),
                        IsErr(ErrorKind::kInvalidConfig))
                << variant.protocol << " size " << size;
        }
        EXPECT_THAT(BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(FlashOperation::kWrite, variant.protocol, variant.mcu,
                                                                 std::nullopt),
                    IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruUnisiaJecsM32rBootModePlan, RejectsReadAndTestWrite)
{
    for (const FlashOperation operation : {FlashOperation::kRead, FlashOperation::kTestWrite})
    {
        EXPECT_THAT(BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(operation, "sub_ecu_unisia_jecs_20_bootmode",
                                                                "M32R_128KB", bytes::Bytes(1, 0x00)),
                    IsErr(ErrorKind::kUnsupported));
        EXPECT_THAT(BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(operation, "sub_ecu_unisia_jecs_20_bootmode",
                                                                 "M32R_128KB", bytes::Bytes(0x20000, 0x00)),
                    IsErr(ErrorKind::kUnsupported));
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
        EXPECT_THAT(
            BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(FlashOperation::kWrite, protocol, mcu, bytes::Bytes(1, 0x00)),
            IsErr(ErrorKind::kInvalidConfig))
            << protocol << " / " << mcu;
    }
}

// Presence means granted: a plan assembled without the confirmation (never
// through the builders) must not validate, so no executor raises the lines.
TEST(SubaruUnisiaJecsM32rBootModePlan, ValidatorRejectsAPlanWithoutTheVoltageConfirmation)
{
    auto plan = ValidateAndBuild(FlashPlanFields{
        .operation = FlashOperation::kWrite,
        .family = FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel,
        .transport = TransportKind::kKline,
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
    EXPECT_THAT(ValidateSubaruUnisiaJecsM32rBootmodePlan(*plan), IsErr(ErrorKind::kInvalidConfig));
}
} // namespace
} // namespace fastecu::flash
