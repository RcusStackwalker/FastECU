// src/backend/flash/flash_types_test.cpp
#include "src/backend/flash/flash_types.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

TEST(FlashTypesTest, FamilyPlanHoldsKlineVariant)
{
    FamilyPlan plan = DensoSh705xEepromKlinePlan{
        .mode = EepromReadMode::Mode2,
        .security = DensoSecurityVariant::Stock,
        .tester_id = 0xf0,
        .target_id = 0x10,
        .initial_baud = 4800,
        .kernel_baud = 15625,
    };

    ASSERT_TRUE(std::holds_alternative<DensoSh705xEepromKlinePlan>(plan));
    EXPECT_EQ(std::get<DensoSh705xEepromKlinePlan>(plan).tester_id, 0xf0);
}

TEST(FlashTypesTest, FamilyPlanHoldsCanVariant)
{
    FamilyPlan plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::Mode3,
        .security = DensoSecurityVariant::EcuTek,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    ASSERT_TRUE(std::holds_alternative<DensoSh705xEepromCanPlan>(plan));
    EXPECT_EQ(std::get<DensoSh705xEepromCanPlan>(plan).response_id, 0x7e8u);
}

TEST(FlashTypesTest, KernelImageOwnsItsBytesIndependently)
{
    bytes::Bytes source{0x01, 0x02, 0x03};
    KernelImage kernel{.id = "sh705x-stock", .load_address = 0xffff2000, .bytes = source};

    source[0] = 0xff;

    EXPECT_EQ(kernel.bytes[0], 0x01);
}

TEST(FlashTypesTest, EepromReadModeValuesMatchProtocolBytes)
{
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::Mode2), 0x02);
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::Mode3), 0x03);
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::Mode4), 0x04);
}

} // namespace
} // namespace fastecu::flash
