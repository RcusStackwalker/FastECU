#include "src/backend/ports/testing/result_matchers.h"
// tests/test_eeprom_read_plan_goldens.cpp
//
// Characterization goldens for the EEPROM read plan (step 5d-6). Written
// against the former desktop implementation before the portable use case
// existed, so that re-pointing them at build_eeprom_read_plan proves the
// conversion preserved behavior. Expected values are hand-derived from the
// protocols' built-in catalog entries (formerly protocols.cfg), which kCan and
// kKline copy, not captured from either implementation's output.
#include "src/backend/flash/eeprom/eeprom_read_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/backend/config/catalog.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

using ::testing::ElementsAre;

namespace fastecu::flash
{
namespace
{

constexpr config::ProtocolSpec kCan{.name = "sub_ecu_eeprom_denso_sh7058_can",
                                    .mcu = "SH7058",
                                    .kernel = "ssmk_can_tp_sh7058.bin",
                                    .kernel_load_address = 0xFFFF3000U};
constexpr config::ProtocolSpec kKline{.name = "sub_ecu_eeprom_denso_sh7055_kline",
                                      .mcu = "SH7055",
                                      .kernel = "ssmk_kline_sh7055.bin",
                                      .kernel_load_address = 0xFFFF6004U};

config::ConfigPaths TestPaths()
{
    config::ConfigPaths paths;
    paths.kernel_files_directory = "kernels/";
    return paths;
}

TEST(EepromReadPlanGolden, Sh7058CanMode2)
{
    InMemoryFileRepository repository;
    repository.files["kernels/ssmk_can_tp_sh7058.bin"] = {0x01, 0x02, 0x03};

    auto plan = BuildEepromReadPlan(TestPaths(), kCan, EepromReadMode::kMode2, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->Operation(), FlashOperation::kRead);
    EXPECT_EQ(plan->Family(), FlashFamily::kDensoSh705xEepromCan);
    EXPECT_EQ(plan->Transport(), TransportKind::kCanIso15765);
    EXPECT_EQ(plan->TargetId(), "sub_ecu_eeprom_denso_sh7058_can");
    EXPECT_EQ(plan->McuName(), "SH7058");
    ASSERT_TRUE(plan->Kernel().has_value());
    EXPECT_EQ(plan->Kernel()->load_address, 0xFFFF3000U);
    EXPECT_THAT(plan->Kernel()->bytes, ElementsAre(0x01, 0x02, 0x03));
    // The kernel is the only file read.
    EXPECT_EQ(repository.read_handles, (std::vector<std::string>{"kernels/ssmk_can_tp_sh7058.bin"}));

    // denso_sh705x_eeprom_common.cpp build_denso_sh705x_eeprom_plan: CAN
    // family_plan is DensoSh705xEepromCanPlan with request_id=0x7e0,
    // response_id=0x7e8, bitrate=500000, extended_id=false. FlashMethod
    // carries no security suffix here, so security_for_protocol falls
    // through to DensoSecurityVariant::kStock.
    const auto *can_plan = std::get_if<DensoSh705xEepromCanPlan>(&plan->FamilyPlan());
    ASSERT_NE(can_plan, nullptr);
    EXPECT_EQ(can_plan->mode, EepromReadMode::kMode2);
    EXPECT_EQ(can_plan->security, DensoSecurityVariant::kStock);
    EXPECT_EQ(can_plan->request_id, 0x7e0U);
    EXPECT_EQ(can_plan->response_id, 0x7e8U);
    EXPECT_EQ(can_plan->bitrate, 500000);
    EXPECT_FALSE(can_plan->extended_id);

    // confirmations_for_mode(Mode2): two entries, no CycleIgnition.
    ASSERT_EQ(plan->Confirmations().size(), 2U);
    EXPECT_EQ(plan->Confirmations()[0].id, ConfirmationSpec::Id::kBeginEepromRead);
    EXPECT_EQ(plan->Confirmations()[1].id, ConfirmationSpec::Id::kInspectEepromBytes);
}

TEST(EepromReadPlanGolden, Sh7055KlineMode4)
{
    InMemoryFileRepository repository;
    repository.files["kernels/ssmk_kline_sh7055.bin"] = {0xaa, 0xbb};

    auto plan = BuildEepromReadPlan(TestPaths(), kKline, EepromReadMode::kMode4, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->Operation(), FlashOperation::kRead);
    EXPECT_EQ(plan->Family(), FlashFamily::kDensoSh705xEepromKline);
    EXPECT_EQ(plan->Transport(), TransportKind::kKline);
    EXPECT_EQ(plan->TargetId(), "sub_ecu_eeprom_denso_sh7055_kline");
    EXPECT_EQ(plan->McuName(), "SH7055");
    ASSERT_TRUE(plan->Kernel().has_value());
    EXPECT_EQ(plan->Kernel()->load_address, 0xFFFF6004U);
    EXPECT_THAT(plan->Kernel()->bytes, ElementsAre(0xaa, 0xbb));
    EXPECT_EQ(repository.read_handles, (std::vector<std::string>{"kernels/ssmk_kline_sh7055.bin"}));

    // denso_sh705x_eeprom_common.cpp build_denso_sh705x_eeprom_plan: K-Line
    // family_plan is DensoSh705xEepromKlinePlan with tester_id=0xf0,
    // target_id=0x10, initial_baud=4800, kernel_baud=15625 (the resolved
    // family value). FlashMethod carries no security suffix here, so
    // security_for_protocol falls through to DensoSecurityVariant::kStock.
    const auto *kline_plan = std::get_if<DensoSh705xEepromKlinePlan>(&plan->FamilyPlan());
    ASSERT_NE(kline_plan, nullptr);
    EXPECT_EQ(kline_plan->mode, EepromReadMode::kMode4);
    EXPECT_EQ(kline_plan->security, DensoSecurityVariant::kStock);
    EXPECT_EQ(kline_plan->tester_id, 0xf0);
    EXPECT_EQ(kline_plan->target_id, 0x10);
    EXPECT_EQ(kline_plan->initial_baud, 4800);
    EXPECT_EQ(kline_plan->kernel_baud, 15625);

    // confirmations_for_mode(Mode4): three entries, CycleIgnition inserted
    // between the begin/inspect pair (the non-Mode2 branch).
    ASSERT_EQ(plan->Confirmations().size(), 3U);
    EXPECT_EQ(plan->Confirmations()[0].id, ConfirmationSpec::Id::kBeginEepromRead);
    EXPECT_EQ(plan->Confirmations()[1].id, ConfirmationSpec::Id::kCycleIgnition);
    EXPECT_EQ(plan->Confirmations()[2].id, ConfirmationSpec::Id::kInspectEepromBytes);
}

} // namespace
} // namespace fastecu::flash
