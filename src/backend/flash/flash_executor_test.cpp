#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/flash_executor.h"

#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

TEST(TransportConfigProjectionTest, CopiesIso15765WireFields)
{
    constexpr SubaruHitachiM32rCanPlan kPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    constexpr Iso15765Config kConfig = iso15765_config_from(kPlan);

    EXPECT_EQ(kConfig.bitrate, 500000);
    EXPECT_EQ(kConfig.request_id, 0x7e0U);
    EXPECT_EQ(kConfig.response_id, 0x7e8U);
    EXPECT_FALSE(kConfig.extended_id);
}

TEST(TransportConfigProjectionTest, CopiesNonIso14230KlineWireFields)
{
    constexpr SubaruMitsuM32rKlinePlan kPlan{
        .tester_id = 0xf0,
        .target_id = 0x10,
        .initial_baud = 4800,
        .flash_baud = 62500,
        .chunk_size = 0x80,
        .unread_prefix_fill = 0x00,
    };

    constexpr KlineConfig kConfig = non_iso14230_kline_config_from(kPlan);

    EXPECT_EQ(kConfig.baud, 4800);
    EXPECT_FALSE(kConfig.iso14230);
    EXPECT_EQ(kConfig.tester_id, 0xf0);
    EXPECT_EQ(kConfig.target_id, 0x10);
}

FlashPlanFields kline_read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromKline,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0xf000, .length = 0x1000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff2000, .bytes = {0x01}},
        .family_plan =
            DensoSh705xEepromKlinePlan{
                .mode = EepromReadMode::kMode2,
                .security = DensoSecurityVariant::kStock,
                .tester_id = 0xf0,
                .target_id = 0x10,
                .initial_baud = 4800,
                .kernel_baud = 15625,
            },
        .confirmations =
            {
                ConfirmationSpec{.id = ConfirmationSpec::Id::kBeginEepromRead},
                ConfirmationSpec{.id = ConfirmationSpec::Id::kInspectEepromBytes},
            },
    };
}

TEST(CheckFamilyTest, MatchingFamilyPasses)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    EXPECT_THAT(check_family(*plan, FlashFamily::kDensoSh705xEepromKline), fastecu::testing::IsOk());
}

TEST(CheckFamilyTest, WrongFamilyFailsWithInvalidConfig)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ASSERT_THAT(check_family(*plan, FlashFamily::kMitsuColtM32rCan),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

} // namespace
} // namespace fastecu::flash
