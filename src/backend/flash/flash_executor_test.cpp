#include "src/backend/flash/flash_executor.h"

#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

TEST(TransportConfigProjectionTest, CopiesIso15765WireFields)
{
    constexpr SubaruHitachiM32rCanPlan plan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    constexpr Iso15765Config config = iso15765_config_from(plan);

    EXPECT_EQ(config.bitrate, 500000);
    EXPECT_EQ(config.request_id, 0x7e0U);
    EXPECT_EQ(config.response_id, 0x7e8U);
    EXPECT_FALSE(config.extended_id);
}

TEST(TransportConfigProjectionTest, CopiesNonIso14230KlineWireFields)
{
    constexpr SubaruMitsuM32rKlinePlan plan{
        .tester_id = 0xf0,
        .target_id = 0x10,
        .initial_baud = 4800,
        .flash_baud = 62500,
        .chunk_size = 0x80,
        .unread_prefix_fill = 0x00,
    };

    constexpr KlineConfig config = non_iso14230_kline_config_from(plan);

    EXPECT_EQ(config.baud, 4800);
    EXPECT_FALSE(config.iso14230);
    EXPECT_EQ(config.tester_id, 0xf0);
    EXPECT_EQ(config.target_id, 0x10);
}

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
