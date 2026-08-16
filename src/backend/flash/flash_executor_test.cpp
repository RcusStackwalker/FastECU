#include "src/backend/flash/flash_executor.h"

#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"

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

TEST(CheckFamilyTransportMatchTest, MatchingFamilyAndTransportPasses)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_TRUE(plan.has_value());

    auto status = check_family_transport_match(*plan, FlashFamily::DensoSh705xEepromKline, TransportKind::Kline);

    EXPECT_TRUE(status.has_value());
}

TEST(CheckFamilyTransportMatchTest, WrongFamilyFailsWithInvalidConfig)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_TRUE(plan.has_value());

    auto status = check_family_transport_match(*plan, FlashFamily::DensoSh705xEepromCan, TransportKind::CanIso15765);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::InvalidConfig);
}

TEST(CheckFamilyTransportMatchTest, WrongTransportFailsWithInvalidConfig)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_TRUE(plan.has_value());

    auto status = check_family_transport_match(*plan, FlashFamily::DensoSh705xEepromKline, TransportKind::CanIso15765);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::InvalidConfig);
}

constexpr Iso15765Config kConfig{
    .bitrate = 500000,
    .request_id = 0x7e1,
    .response_id = 0x7e9,
    .extended_id = false,
};

TEST(OpenCanIso15765TransportTest, ReturnsTheCanTransportConfiguredAndOpen)
{
    ScriptedCanFlashTransport can;
    IFlashTransport& transport = can;

    Result<ICanFlashTransport *> opened = open_can_iso15765_transport(transport, kConfig);

    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(*opened, &can);
    ASSERT_TRUE(can.last_config_.has_value());
    EXPECT_EQ(can.last_config_->request_id, kConfig.request_id);
    EXPECT_EQ(can.last_config_->response_id, kConfig.response_id);
}

TEST(OpenCanIso15765TransportTest, FailsWithInvalidConfigWhenTransportIsNotCan)
{
    ScriptedKlineFlashTransport kline;
    IFlashTransport& transport = kline;

    Result<ICanFlashTransport *> opened = open_can_iso15765_transport(transport, kConfig);

    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().kind, ErrorKind::InvalidConfig);
}

TEST(OpenCanIso15765TransportTest, PropagatesAConfigureFailure)
{
    ScriptedCanFlashTransport can;
    can.configure_result_ = fail(ErrorKind::Disconnected, "configure failed");
    IFlashTransport& transport = can;

    Result<ICanFlashTransport *> opened = open_can_iso15765_transport(transport, kConfig);

    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().kind, ErrorKind::Disconnected);
}

TEST(OpenCanIso15765TransportTest, PropagatesAnOpenFailure)
{
    ScriptedCanFlashTransport can;
    can.open_result_ = fail(ErrorKind::Disconnected, "open failed");
    IFlashTransport& transport = can;

    Result<ICanFlashTransport *> opened = open_can_iso15765_transport(transport, kConfig);

    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().kind, ErrorKind::Disconnected);
}

} // namespace
} // namespace fastecu::flash
