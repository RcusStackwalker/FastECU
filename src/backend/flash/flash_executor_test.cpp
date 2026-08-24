#include "src/backend/flash/flash_executor.h"

#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

FlashPlanFields kline_read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromKline,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0xf000, .length = 0x1000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff2000, .bytes = {0x01}},
        .family_plan =
            DensoSh705xEepromKlinePlan{
                .mode = EepromReadMode::Mode2,
                .security = DensoSecurityVariant::Stock,
                .tester_id = 0xf0,
                .target_id = 0x10,
                .initial_baud = 4800,
                .kernel_baud = 15625,
            },
        .confirmations =
            {
                ConfirmationSpec{.id = ConfirmationSpec::Id::BeginEepromRead},
                ConfirmationSpec{.id = ConfirmationSpec::Id::InspectEepromBytes},
            },
    };
}

TEST(CheckFamilyTest, MatchingFamilyPasses)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_TRUE(plan.has_value());

    EXPECT_TRUE(check_family(*plan, FlashFamily::DensoSh705xEepromKline).has_value());
}

TEST(CheckFamilyTest, WrongFamilyFailsWithInvalidConfig)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_TRUE(plan.has_value());

    auto status = check_family(*plan, FlashFamily::MitsuColtM32rCan);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::InvalidConfig);
}

} // namespace
} // namespace fastecu::flash
