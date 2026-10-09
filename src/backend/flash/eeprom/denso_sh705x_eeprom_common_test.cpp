#include "src/backend/ports/testing/result_matchers.h"
// src/backend/flash/eeprom/denso_sh705x_eeprom_common_test.cpp
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

#include <gtest/gtest.h>

#include <limits>

namespace fastecu::flash
{
namespace
{

// Literal values transcribed from src/backend/flash/kernel/kernelmemorymodels.h:
//   kEepromBlocksSH7055[0] = {0x00000000, 0x00000100} (line 280)
//   kKernelBlocksSH7055[0] = {0xFFFF6004, 0x00006000} (line 276)
// Do not derive these from anywhere else; the MCU table is the single source
// of truth the implementation also reads from.
constexpr std::uint32_t kSh7055EepromStart = 0x00000000;
constexpr std::uint32_t kSh7055EepromLen = 0x00000100;
constexpr std::uint32_t kSh7055KernelRamStart = 0xFFFF6004;
constexpr std::uint32_t kSh7055KernelRamLen = 0x00006000;
constexpr std::uint32_t kSh7055KernelRamEnd = kSh7055KernelRamStart + kSh7055KernelRamLen;

DensoSh705xEepromInput valid_kline_input(EepromReadMode mode = EepromReadMode::kMode2)
{
    return DensoSh705xEepromInput{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel =
            KernelImage{
                .id = "sh705x-kernel",
                .load_address = kSh7055KernelRamStart,
                .bytes = bytes::Bytes(64, 0xaa),
            },
        .mode = mode,
        .security = DensoSecurityVariant::kStock,
        .eeprom_region = MemoryRegion{.start = kSh7055EepromStart, .length = kSh7055EepromLen},
    };
}

TEST(DensoSh705xEepromCommonTest, ValidKlineMode2ProducesReadPlanWithTwoConfirmations)
{
    auto plan = build_denso_sh705x_eeprom_plan(valid_kline_input());

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->operation(), FlashOperation::kRead);
    EXPECT_EQ(plan->transport(), TransportKind::kKline);
    ASSERT_EQ(plan->confirmations().size(), 2U);
    EXPECT_EQ(plan->confirmations()[0].id, ConfirmationSpec::Id::kBeginEepromRead);
    EXPECT_EQ(plan->confirmations()[1].id, ConfirmationSpec::Id::kInspectEepromBytes);

    ASSERT_TRUE(std::holds_alternative<DensoSh705xEepromKlinePlan>(plan->family_plan()));
    const auto& kline_plan = std::get<DensoSh705xEepromKlinePlan>(plan->family_plan());
    EXPECT_EQ(kline_plan.tester_id, 0xf0);
    EXPECT_EQ(kline_plan.target_id, 0x10);
    EXPECT_EQ(kline_plan.initial_baud, 4800);
}

TEST(DensoSh705xEepromCommonTest, Mode3And4AddCycleIgnitionConfirmation)
{
    for (EepromReadMode mode : {EepromReadMode::kMode3, EepromReadMode::kMode4})
    {
        auto plan = build_denso_sh705x_eeprom_plan(valid_kline_input(mode));

        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ASSERT_EQ(plan->confirmations().size(), 3U);
        EXPECT_EQ(plan->confirmations()[0].id, ConfirmationSpec::Id::kBeginEepromRead);
        EXPECT_EQ(plan->confirmations()[1].id, ConfirmationSpec::Id::kCycleIgnition);
        EXPECT_EQ(plan->confirmations()[2].id, ConfirmationSpec::Id::kInspectEepromBytes);
    }
}

TEST(DensoSh705xEepromCommonTest, WriteOperationIsUnsupported)
{
    auto input = valid_kline_input();
    input.operation = FlashOperation::kWrite;

    ASSERT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsErr(ErrorKind::kUnsupported));
}

TEST(DensoSh705xEepromCommonTest, TestWriteOperationIsUnsupported)
{
    auto input = valid_kline_input();
    input.operation = FlashOperation::kTestWrite;

    EXPECT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsErr(ErrorKind::kUnsupported));
}

TEST(DensoSh705xEepromCommonTest, KlineRejectsCobbSecurity)
{
    auto input = valid_kline_input();
    input.security = DensoSecurityVariant::kCobb;

    EXPECT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DensoSh705xEepromCommonTest, KlineRejectsEcuTekRaceRomSecurity)
{
    auto input = valid_kline_input();
    input.security = DensoSecurityVariant::kEcuTekRaceRom;

    EXPECT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DensoSh705xEepromCommonTest, KlineAcceptsEcuTekSecurity)
{
    auto input = valid_kline_input();
    input.security = DensoSecurityVariant::kEcuTek;

    EXPECT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsOk());
}

TEST(DensoSh705xEepromCommonTest, CanAcceptsAllFourSecurityVariants)
{
    for (DensoSecurityVariant security : {DensoSecurityVariant::kStock, DensoSecurityVariant::kEcuTek,
                                          DensoSecurityVariant::kCobb, DensoSecurityVariant::kEcuTekRaceRom})
    {
        auto input = valid_kline_input();
        input.family = FlashFamily::kDensoSh705xEepromCan;
        input.security = security;

        EXPECT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsOk())
            << "security variant " << static_cast<int>(security) << " should be accepted on CAN";
    }
}

TEST(DensoSh705xEepromCommonTest, EepromRegionMismatchIsRejected)
{
    auto input = valid_kline_input();
    input.eeprom_region.length += 1;

    EXPECT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DensoSh705xEepromCommonTest, KernelLoadAddressOutsideRamRangeIsRejected)
{
    auto input = valid_kline_input();
    input.kernel.load_address = kSh7055KernelRamStart + kSh7055KernelRamLen + 1;

    EXPECT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DensoSh705xEepromCommonTest, KlineRejectsRawKernelThatFitsButWireFootprintCrossesRamEnd)
{
    auto input = valid_kline_input();
    // The raw, already-four-byte-aligned payload ends exactly at ram_end;
    // only the required four-byte bypass trailer crosses the boundary.
    input.kernel.load_address = kSh7055KernelRamEnd - 4;
    input.kernel.bytes = {0xaa, 0xbb, 0xcc, 0xdd};

    ASSERT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DensoSh705xEepromCommonTest, CanRejectsRawKernelThatFitsButWireFootprintCrossesRamEnd)
{
    auto input = valid_kline_input();
    input.family = FlashFamily::kDensoSh705xEepromCan;
    // Four raw bytes end exactly at ram_end, but the CAN wire payload is a
    // complete 128-byte block.
    input.kernel.load_address = kSh7055KernelRamEnd - 4;
    input.kernel.bytes = {0xaa, 0xbb, 0xcc, 0xdd};

    ASSERT_THAT(build_denso_sh705x_eeprom_plan(input), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DensoSh705xEepromCommonTest, PreflightRejectsKernelSizeWhoseWireFootprintOverflows)
{
    auto input = valid_kline_input();

    ASSERT_THAT(validate_denso_sh705x_eeprom_preflight(input, std::numeric_limits<std::size_t>::max()),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DensoSh705xEepromCommonTest, ResolveSh705xEepromRegionReturnsKnownMcuBounds)
{
    // Exercised directly because build_eeprom_read_plan calls this exported
    // function to avoid keeping its own copy of these literals; a regression
    // here would silently break that caller too.
    auto sh7055 = resolve_sh705x_eeprom_region("SH7055");
    ASSERT_THAT(sh7055, fastecu::testing::IsOk());
    EXPECT_EQ(sh7055->start, kSh7055EepromStart);
    EXPECT_EQ(sh7055->length, kSh7055EepromLen);

    auto sh7058 = resolve_sh705x_eeprom_region("SH7058");
    ASSERT_THAT(sh7058, fastecu::testing::IsOk());
    EXPECT_EQ(sh7058->start, 0x00000000U);
    EXPECT_EQ(sh7058->length, 0x00000100U);
}

TEST(DensoSh705xEepromCommonTest, ResolveSh705xEepromRegionRejectsUnknownMcu)
{
    ASSERT_THAT(resolve_sh705x_eeprom_region("NOT_A_REAL_MCU"), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(DensoSh705xEepromCommonTest, NoTransportOrConfigurationCallOccursOnRejection)
{
    // Structural guarantee, not a mock assertion: build_denso_sh705x_eeprom_plan
    // takes no transport/executor argument at all, so a rejected input cannot
    // have performed any I/O by construction. This test exists to document
    // that guarantee at the call site future maintainers read first.
    auto input = valid_kline_input();
    input.operation = FlashOperation::kWrite;

    static_assert(std::is_same_v<decltype(build_denso_sh705x_eeprom_plan(input)), Result<FlashPlan>>);
    EXPECT_THAT(build_denso_sh705x_eeprom_plan(input), ::testing::Not(fastecu::testing::IsOk()));
}

} // namespace
} // namespace fastecu::flash
