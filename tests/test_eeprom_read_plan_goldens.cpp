// tests/test_eeprom_read_plan_goldens.cpp
//
// Characterization goldens for the EEPROM read plan (step 5d-6). Written
// against LegacyFlashSnapshotAdapter BEFORE the portable use case existed, so
// that re-pointing them at build_eeprom_read_plan proves the conversion
// preserved behavior. Expected values are hand-derived from
// resources/shared/config/protocols.cfg, not captured from either
// implementation's output.
#include "src/backend/flash/eeprom/eeprom_read_plan.h"

#include <gtest/gtest.h>

#include "src/backend/ports/testing/in_memory_file_repository.h"

namespace fastecu::flash
{
namespace
{

const char *kFixture = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
    <protocols>
        <protocol name="sub_ecu_eeprom_denso_sh7058_can">
            <ecu>Denso SH7058</ecu>
            <mcu>SH7058</mcu>
            <kernel>ssmk_can_tp_sh7058.bin</kernel>
            <kernel_addr>0xFFFF3000</kernel_addr>
        </protocol>
        <protocol name="sub_ecu_eeprom_denso_sh7055_kline">
            <ecu>Denso SH7055</ecu>
            <mcu>SH7055</mcu>
            <kernel>ssmk_kline_sh7055.bin</kernel>
            <kernel_addr>0xFFFF6004</kernel_addr>
        </protocol>
    </protocols>
    <car_models>
        <car_model>
            <make>Subaru</make><model>Impreza</model><version>WRX</version>
            <protocol>sub_ecu_eeprom_denso_sh7055_kline</protocol>
        </car_model>
        <car_model>
            <make>Subaru</make><model>Legacy</model><version>GT</version>
            <protocol>sub_ecu_eeprom_denso_sh7058_can</protocol>
        </car_model>
    </car_models>
</config>)";

config::ConfigPaths test_paths()
{
    config::ConfigPaths paths;
    paths.protocols_file = "protocols.cfg";
    paths.kernel_files_directory = "kernels/";
    return paths;
}

void add_protocols_fixture(InMemoryFileRepository& repository)
{
    const std::string xml(kFixture);
    repository.files["protocols.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());
}

// protocols.cfg: <protocol name="sub_ecu_eeprom_denso_sh7058_can">
//   <mcu>SH7058</mcu>
//   <kernel>ssmk_can_tp_sh7058.bin</kernel>
//   <kernel_addr>0xFFFF3000</kernel_addr>
TEST(EepromReadPlanGolden, Sh7058CanMode2)
{
    InMemoryFileRepository repository;
    add_protocols_fixture(repository);
    repository.files["kernels/ssmk_can_tp_sh7058.bin"] = {0x01, 0x02, 0x03};

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can",
                                       EepromReadMode::Mode2, repository);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->operation(), FlashOperation::Read);
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromCan);
    EXPECT_EQ(plan->transport(), TransportKind::CanIso15765);
    EXPECT_EQ(plan->target_id(), "sub_ecu_eeprom_denso_sh7058_can");
    EXPECT_EQ(plan->mcu_name(), "SH7058");
    EXPECT_EQ(plan->kernel().load_address, 0xFFFF3000u);
    EXPECT_EQ(plan->kernel().bytes, (bytes::Bytes{0x01, 0x02, 0x03}));
    // Each catalog loader reads the shared config before the kernel read.
    EXPECT_EQ(repository.read_handles,
              (std::vector<std::string>{"protocols.cfg", "protocols.cfg",
                                        "kernels/ssmk_can_tp_sh7058.bin"}));

    // denso_sh705x_eeprom_common.cpp build_denso_sh705x_eeprom_plan: CAN
    // family_plan is DensoSh705xEepromCanPlan with request_id=0x7e0,
    // response_id=0x7e8, bitrate=500000, extended_id=false. FlashMethod
    // carries no security suffix here, so security_for_flash_method falls
    // through to DensoSecurityVariant::Stock.
    const auto *can_plan = std::get_if<DensoSh705xEepromCanPlan>(&plan->family_plan());
    ASSERT_NE(can_plan, nullptr);
    EXPECT_EQ(can_plan->mode, EepromReadMode::Mode2);
    EXPECT_EQ(can_plan->security, DensoSecurityVariant::Stock);
    EXPECT_EQ(can_plan->request_id, 0x7e0u);
    EXPECT_EQ(can_plan->response_id, 0x7e8u);
    EXPECT_EQ(can_plan->bitrate, 500000);
    EXPECT_FALSE(can_plan->extended_id);

    // confirmations_for_mode(Mode2): two entries, no CycleIgnition.
    ASSERT_EQ(plan->confirmations().size(), 2u);
    EXPECT_EQ(plan->confirmations()[0].id, ConfirmationSpec::Id::BeginEepromRead);
    EXPECT_EQ(plan->confirmations()[1].id, ConfirmationSpec::Id::InspectEepromBytes);
}

// protocols.cfg: <protocol name="sub_ecu_eeprom_denso_sh7055_kline">
//   <mcu>SH7055</mcu>
//   <kernel>ssmk_kline_sh7055.bin</kernel>
//   <kernel_addr>0xFFFF6004</kernel_addr>
TEST(EepromReadPlanGolden, Sh7055KlineMode4)
{
    InMemoryFileRepository repository;
    add_protocols_fixture(repository);
    repository.files["kernels/ssmk_kline_sh7055.bin"] = {0xaa, 0xbb};

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7055_kline",
                                       EepromReadMode::Mode4, repository);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromKline);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);
    EXPECT_EQ(plan->target_id(), "sub_ecu_eeprom_denso_sh7055_kline");
    EXPECT_EQ(plan->mcu_name(), "SH7055");
    EXPECT_EQ(plan->kernel().load_address, 0xFFFF6004u);

    // denso_sh705x_eeprom_common.cpp build_denso_sh705x_eeprom_plan: K-Line
    // family_plan is DensoSh705xEepromKlinePlan with tester_id=0xf0,
    // target_id=0x10, initial_baud=4800, kernel_baud=15625 (the resolved
    // family value). FlashMethod carries no security suffix here, so
    // security_for_flash_method falls through to DensoSecurityVariant::Stock.
    const auto *kline_plan = std::get_if<DensoSh705xEepromKlinePlan>(&plan->family_plan());
    ASSERT_NE(kline_plan, nullptr);
    EXPECT_EQ(kline_plan->mode, EepromReadMode::Mode4);
    EXPECT_EQ(kline_plan->security, DensoSecurityVariant::Stock);
    EXPECT_EQ(kline_plan->tester_id, 0xf0);
    EXPECT_EQ(kline_plan->target_id, 0x10);
    EXPECT_EQ(kline_plan->initial_baud, 4800);
    EXPECT_EQ(kline_plan->kernel_baud, 15625);

    // confirmations_for_mode(Mode4): three entries, CycleIgnition inserted
    // between the begin/inspect pair (the non-Mode2 branch).
    ASSERT_EQ(plan->confirmations().size(), 3u);
    EXPECT_EQ(plan->confirmations()[0].id, ConfirmationSpec::Id::BeginEepromRead);
    EXPECT_EQ(plan->confirmations()[1].id, ConfirmationSpec::Id::CycleIgnition);
    EXPECT_EQ(plan->confirmations()[2].id, ConfirmationSpec::Id::InspectEepromBytes);
}

} // namespace
} // namespace fastecu::flash
