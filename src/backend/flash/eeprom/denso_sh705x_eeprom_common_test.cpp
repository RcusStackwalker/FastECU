// src/backend/flash/eeprom/denso_sh705x_eeprom_common_test.cpp
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

// Literal values transcribed from src/backend/definitions/kernelmemorymodels.h:
//   eblocks_SH7055[0] = {0x00000000, 0x00000100} (line 280)
//   kblocks_SH7055[0] = {0xFFFF6004, 0x00006000} (line 276)
// Do not derive these from anywhere else; the MCU table is the single source
// of truth the implementation also reads from.
constexpr std::uint32_t kSh7055EepromStart = 0x00000000;
constexpr std::uint32_t kSh7055EepromLen = 0x00000100;
constexpr std::uint32_t kSh7055KernelRamStart = 0xFFFF6004;
constexpr std::uint32_t kSh7055KernelRamLen = 0x00006000;

DensoSh705xEepromInput valid_kline_input(EepromReadMode mode = EepromReadMode::Mode2)
{
    return DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel = KernelImage{
            .id = "sh705x-kernel",
            .load_address = kSh7055KernelRamStart,
            .bytes = bytes::Bytes(64, 0xaa),
        },
        .mode = mode,
        .security = DensoSecurityVariant::Stock,
        .eeprom_region = MemoryRegion{.start = kSh7055EepromStart, .length = kSh7055EepromLen},
    };
}

TEST(DensoSh705xEepromCommonTest, ValidKlineMode2ProducesReadPlanWithTwoConfirmations)
{
    auto plan = build_denso_sh705x_eeprom_plan(valid_kline_input());

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->operation(), FlashOperation::Read);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);
    ASSERT_EQ(plan->confirmations().size(), 2u);
    EXPECT_EQ(plan->confirmations()[0].id, ConfirmationSpec::Id::BeginEepromRead);
    EXPECT_EQ(plan->confirmations()[1].id, ConfirmationSpec::Id::InspectEepromBytes);

    ASSERT_TRUE(std::holds_alternative<DensoSh705xEepromKlinePlan>(plan->family_plan()));
    const auto& kline_plan = std::get<DensoSh705xEepromKlinePlan>(plan->family_plan());
    EXPECT_EQ(kline_plan.tester_id, 0xf0);
    EXPECT_EQ(kline_plan.target_id, 0x10);
    EXPECT_EQ(kline_plan.initial_baud, 4800);
}

TEST(DensoSh705xEepromCommonTest, Mode3And4AddCycleIgnitionConfirmation)
{
    for (EepromReadMode mode : {EepromReadMode::Mode3, EepromReadMode::Mode4})
    {
        auto plan = build_denso_sh705x_eeprom_plan(valid_kline_input(mode));

        ASSERT_TRUE(plan.has_value());
        ASSERT_EQ(plan->confirmations().size(), 3u);
        EXPECT_EQ(plan->confirmations()[0].id, ConfirmationSpec::Id::BeginEepromRead);
        EXPECT_EQ(plan->confirmations()[1].id, ConfirmationSpec::Id::CycleIgnition);
        EXPECT_EQ(plan->confirmations()[2].id, ConfirmationSpec::Id::InspectEepromBytes);
    }
}

TEST(DensoSh705xEepromCommonTest, WriteOperationIsUnsupported)
{
    auto input = valid_kline_input();
    input.operation = FlashOperation::Write;

    auto plan = build_denso_sh705x_eeprom_plan(input);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(DensoSh705xEepromCommonTest, TestWriteOperationIsUnsupported)
{
    auto input = valid_kline_input();
    input.operation = FlashOperation::TestWrite;

    EXPECT_EQ(build_denso_sh705x_eeprom_plan(input).error().kind, ErrorKind::Unsupported);
}

TEST(DensoSh705xEepromCommonTest, KlineRejectsCobbSecurity)
{
    auto input = valid_kline_input();
    input.security = DensoSecurityVariant::Cobb;

    EXPECT_EQ(build_denso_sh705x_eeprom_plan(input).error().kind, ErrorKind::InvalidConfig);
}

TEST(DensoSh705xEepromCommonTest, KlineRejectsEcuTekRaceRomSecurity)
{
    auto input = valid_kline_input();
    input.security = DensoSecurityVariant::EcuTekRaceRom;

    EXPECT_EQ(build_denso_sh705x_eeprom_plan(input).error().kind, ErrorKind::InvalidConfig);
}

TEST(DensoSh705xEepromCommonTest, KlineAcceptsEcuTekSecurity)
{
    auto input = valid_kline_input();
    input.security = DensoSecurityVariant::EcuTek;

    EXPECT_TRUE(build_denso_sh705x_eeprom_plan(input).has_value());
}

TEST(DensoSh705xEepromCommonTest, CanAcceptsAllFourSecurityVariants)
{
    for (DensoSecurityVariant security :
         {DensoSecurityVariant::Stock, DensoSecurityVariant::EcuTek,
          DensoSecurityVariant::Cobb, DensoSecurityVariant::EcuTekRaceRom})
    {
        auto input = valid_kline_input();
        input.family = FlashFamily::DensoSh705xEepromCan;
        input.security = security;

        EXPECT_TRUE(build_denso_sh705x_eeprom_plan(input).has_value())
            << "security variant " << static_cast<int>(security) << " should be accepted on CAN";
    }
}

TEST(DensoSh705xEepromCommonTest, EepromRegionMismatchIsRejected)
{
    auto input = valid_kline_input();
    input.eeprom_region.length += 1;

    EXPECT_EQ(build_denso_sh705x_eeprom_plan(input).error().kind, ErrorKind::InvalidConfig);
}

TEST(DensoSh705xEepromCommonTest, KernelLoadAddressOutsideRamRangeIsRejected)
{
    auto input = valid_kline_input();
    input.kernel.load_address = kSh7055KernelRamStart + kSh7055KernelRamLen + 1;

    EXPECT_EQ(build_denso_sh705x_eeprom_plan(input).error().kind, ErrorKind::InvalidConfig);
}

TEST(DensoSh705xEepromCommonTest, NoTransportOrConfigurationCallOccursOnRejection)
{
    // Structural guarantee, not a mock assertion: build_denso_sh705x_eeprom_plan
    // takes no transport/executor argument at all, so a rejected input cannot
    // have performed any I/O by construction. This test exists to document
    // that guarantee at the call site future maintainers read first.
    auto input = valid_kline_input();
    input.operation = FlashOperation::Write;

    static_assert(std::is_same_v<
                  decltype(build_denso_sh705x_eeprom_plan(input)),
                  Result<FlashPlan>>);
    EXPECT_FALSE(build_denso_sh705x_eeprom_plan(input).has_value());
}

} // namespace
} // namespace fastecu::flash
