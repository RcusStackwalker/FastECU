// src/backend/flash/flash_types_test.cpp
#include "src/backend/flash/flash_types.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

#include <gtest/gtest.h>

#include <any>

namespace fastecu::flash
{
namespace
{

TEST(FlashTypesTest, FamilyPlanHoldsKlineVariant)
{
    FamilyPlan plan = DensoSh705xEepromKlinePlan{
        .mode = EepromReadMode::kMode2,
        .security = DensoSecurityVariant::kStock,
        .tester_id = 0xf0,
        .target_id = 0x10,
        .initial_baud = 4800,
        .kernel_baud = 15625,
    };

    ASSERT_TRUE(plan.As<DensoSh705xEepromKlinePlan>() != nullptr);
    EXPECT_EQ(plan.Get<DensoSh705xEepromKlinePlan>().tester_id, 0xf0);
}

TEST(FlashTypesTest, FamilyPlanHoldsCanVariant)
{
    FamilyPlan plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::kMode3,
        .security = DensoSecurityVariant::kEcuTek,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    ASSERT_TRUE(plan.As<DensoSh705xEepromCanPlan>() != nullptr);
    EXPECT_EQ(plan.Get<DensoSh705xEepromCanPlan>().response_id, 0x7e8U);
}

TEST(FlashTypesTest, MixedCanTransportKindIsDistinctFromIso15765)
{
    EXPECT_NE(TransportKind::kCanRawIso15765, TransportKind::kCanIso15765);
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

    ASSERT_TRUE(plan.As<MitsuColtM32rCanPlan>() != nullptr);
    const auto *colt_plan = plan.As<MitsuColtM32rCanPlan>();
    ASSERT_NE(colt_plan, nullptr);
    EXPECT_EQ(colt_plan->request_id, 0x7e0U);
    EXPECT_EQ(colt_plan->response_id, 0x7e8U);
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
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::kMode2), 0x02);
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::kMode3), 0x03);
    EXPECT_EQ(static_cast<std::uint8_t>(EepromReadMode::kMode4), 0x04);
}

TEST(FlashTypesTest, FamilyPlanRecordsTheTraitsOfThePlanTypeItWraps)
{
    const FamilyPlan plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::kMode2,
        .security = DensoSecurityVariant::kStock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    EXPECT_TRUE(plan.HasValue());
    EXPECT_EQ(plan.Family(), FlashFamily::kDensoSh705xEepromCan);
    EXPECT_EQ(plan.Transport(), TransportKind::kCanIso15765);
    EXPECT_TRUE(plan.RequiresKernel());
}

TEST(FlashTypesTest, FamilyPlanReadsBackOnlyItsOwnPlanType)
{
    // Hitachi SH7058 is one flash family with two plan types.
    const FamilyPlan plan = SubaruHitachiSh7058KlinePlan{};

    EXPECT_NE(plan.As<SubaruHitachiSh7058KlinePlan>(), nullptr);
    EXPECT_EQ(plan.As<SubaruHitachiSh7058CanPlan>(), nullptr);
    EXPECT_THROW((void)plan.Get<SubaruHitachiSh7058CanPlan>(), std::bad_any_cast);
}

TEST(FlashTypesTest, DefaultFamilyPlanIsEmpty)
{
    const FamilyPlan plan;

    EXPECT_FALSE(plan.HasValue());
    EXPECT_EQ(plan.As<SubaruHitachiSh7058KlinePlan>(), nullptr);
}

TEST(FlashTypes, DensoMc68AndSh7055FamilyPlansConstructAndHoldValues)
{
    SubaruDensoMc68hc16y5_02Plan mc68{
        .connect_baud = 9600,
        .kernel_baud = 11700,
        .encryption_xor = 0x51,
        .kernel_magic = 0x3940,
        .bootloader_ok = {0x4C, 0x00, 0xB4},
    };
    FamilyPlan mc68_variant = mc68;
    EXPECT_EQ(mc68_variant.Get<SubaruDensoMc68hc16y5_02Plan>().kernel_baud, 11700);

    SubaruDensoSh7055_02Plan sh7055{.tester_id = 0xf0, .target_id = 0x10, .read_ecu_id = true};
    FamilyPlan sh7055_variant = sh7055;
    EXPECT_TRUE(sh7055_variant.Get<SubaruDensoSh7055_02Plan>().read_ecu_id);

    EXPECT_TRUE(FamilyTraits<SubaruDensoMc68hc16y5_02Plan>::kRequiresKernel);
    EXPECT_TRUE(FamilyTraits<SubaruDensoSh7055_02Plan>::kRequiresKernel);
}

TEST(FlashTypes, BootloaderDialectFamilyPlansAreKernelFree)
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
    EXPECT_EQ(denso_variant.Get<SubaruDenso1n83m_1_5mCanPlan>().lead_pad_len, 0x10000U);

    EXPECT_FALSE(FamilyTraits<SubaruDenso1n83m_1_5mCanPlan>::kRequiresKernel);

    SubaruDensoSh72531CanPlan sh72531{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .lead_pad_len = 0x8000,
        .tail_pad_len = 0x100,
    };
    FamilyPlan sh72531_variant = sh72531;
    EXPECT_EQ(sh72531_variant.Get<SubaruDensoSh72531CanPlan>().lead_pad_len, 0x8000U);

    EXPECT_FALSE(FamilyTraits<SubaruDensoSh72531CanPlan>::kRequiresKernel);

    SubaruDensoSh72543CanDieselPlan sh72543{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .lead_pad_len = 0x8000,
        .tail_pad_len = 0x100,
    };
    FamilyPlan sh72543_variant = sh72543;
    EXPECT_EQ(sh72543_variant.Get<SubaruDensoSh72543CanDieselPlan>().lead_pad_len, 0x8000U);

    EXPECT_FALSE(FamilyTraits<SubaruDensoSh72543CanDieselPlan>::kRequiresKernel);

    SubaruDenso1n83m_4mCanPlan denso4m{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .lead_pad_len = 0x10000,
        .tail_pad_len = 0x100,
    };
    FamilyPlan denso4m_variant = denso4m;
    EXPECT_EQ(denso4m_variant.Get<SubaruDenso1n83m_4mCanPlan>().lead_pad_len, 0x10000U);

    EXPECT_FALSE(FamilyTraits<SubaruDenso1n83m_4mCanPlan>::kRequiresKernel);
}

} // namespace
} // namespace fastecu::flash
