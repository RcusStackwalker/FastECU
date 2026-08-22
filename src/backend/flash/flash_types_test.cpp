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

TEST(FlashTypesTest, FamilyPlanHoldsMitsuColtM32rCanVariant)
{
    FamilyPlan plan = MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = true,
        .session_id = 0x85,
    };

    ASSERT_TRUE(std::holds_alternative<MitsuColtM32rCanPlan>(plan));
    const auto *colt_plan = std::get_if<MitsuColtM32rCanPlan>(&plan);
    ASSERT_NE(colt_plan, nullptr);
    EXPECT_EQ(colt_plan->request_id, 0x7e0u);
    EXPECT_EQ(colt_plan->response_id, 0x7e8u);
    EXPECT_EQ(colt_plan->bitrate, 500000);
    EXPECT_FALSE(colt_plan->extended_id);
    EXPECT_TRUE(colt_plan->use_vendor_challenge);
    EXPECT_EQ(colt_plan->session_id, 0x85);
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

TEST(FlashTypesTest, FamilyRequiresKernelDefaultsTrueForExistingFamilies)
{
    EXPECT_TRUE(family_requires_kernel_v<DensoSh705xEepromKlinePlan>);
    EXPECT_TRUE(family_requires_kernel_v<DensoSh705xEepromCanPlan>);
}

TEST(FlashTypes, Wave2FamilyPlansConstructAndHoldValues)
{
    SubaruDensoMc68hc16y5_02Plan mc68{
        .connect_baud = 9600,
        .kernel_baud = 11700,
        .encryption_xor = 0x51,
        .kernel_magic = 0x3940,
        .bootloader_ok = {0x4C, 0x00, 0xB4},
    };
    FamilyPlan mc68_variant = mc68;
    EXPECT_EQ(std::get<SubaruDensoMc68hc16y5_02Plan>(mc68_variant).kernel_baud, 11700);

    SubaruDensoSh7055_02Plan sh7055{.tester_id = 0xf0, .target_id = 0x10, .read_ecu_id = true};
    FamilyPlan sh7055_variant = sh7055;
    EXPECT_TRUE(std::get<SubaruDensoSh7055_02Plan>(sh7055_variant).read_ecu_id);

    EXPECT_TRUE(family_requires_kernel_v<SubaruDensoMc68hc16y5_02Plan>);
    EXPECT_TRUE(family_requires_kernel_v<SubaruDensoSh7055_02Plan>);
}

TEST(FlashTypes, Wave4DensoIso15765FamilyPlansAreKernelFree)
{
    SubaruDenso1n83m_1_5mCanPlan denso{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .lead_pad_len = 0x10000,
        .tail_pad_len = 0x100,
    };
    FamilyPlan denso_variant = denso;
    EXPECT_EQ(std::get<SubaruDenso1n83m_1_5mCanPlan>(denso_variant).lead_pad_len, 0x10000u);

    EXPECT_FALSE(family_requires_kernel_v<SubaruDenso1n83m_1_5mCanPlan>);
}

} // namespace
} // namespace fastecu::flash
