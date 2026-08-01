// tests/test_eeprom_read_plan_goldens.cpp
//
// Characterization goldens for the EEPROM read plan (step 5d-6). Written
// against LegacyFlashSnapshotAdapter BEFORE the portable use case exists, so
// that re-pointing them at build_eeprom_read_plan proves the conversion
// preserved behavior. Expected values are hand-derived from
// resources/shared/config/protocols.cfg, not captured from either
// implementation's output.
#include "src/platform/desktop/common/flash/flash_snapshot_adapter.h"

#include <gtest/gtest.h>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

namespace fastecu::flash
{
namespace
{

// protocols.cfg: <protocol name="sub_ecu_eeprom_denso_sh7058_can">
//   <mcu>SH7058</mcu>
//   <kernel>ssmk_can_tp_sh7058.bin</kernel>
//   <kernel_addr>0xFFFF3000</kernel_addr>
TEST(EepromReadPlanGolden, Sh7058CanMode2)
{
    InMemoryFileRepository repository;
    repository.files["kernels/ssmk_can_tp_sh7058.bin"] = {0x01, 0x02, 0x03};
    LegacyFlashSnapshotAdapter adapter(repository);

    FileActions::EcuCalDefStructure ecu_cal_def;
    ecu_cal_def.McuType = "SH7058";
    ecu_cal_def.FlashMethod = "sub_ecu_eeprom_denso_sh7058_can";
    ecu_cal_def.KernelStartAddr = "0xFFFF3000";

    auto plan = adapter.build_read_plan(ecu_cal_def, "sub_ecu_eeprom_denso_sh7058_can",
                                        EepromReadMode::Mode2,
                                        "kernels/ssmk_can_tp_sh7058.bin");

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->operation(), FlashOperation::Read);
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromCan);
    EXPECT_EQ(plan->transport(), TransportKind::CanIso15765);
    EXPECT_EQ(plan->target_id(), "sub_ecu_eeprom_denso_sh7058_can");
    EXPECT_EQ(plan->mcu_name(), "SH7058");
    EXPECT_EQ(plan->kernel().load_address, 0xFFFF3000u);
    EXPECT_EQ(plan->kernel().bytes, (bytes::Bytes{0x01, 0x02, 0x03}));
    EXPECT_EQ(repository.read_handles,
              std::vector<std::string>{"kernels/ssmk_can_tp_sh7058.bin"});
}

// protocols.cfg: <protocol name="sub_ecu_eeprom_denso_sh7055_kline">
//   <mcu>SH7055</mcu>
//   <kernel>ssmk_kline_sh7055.bin</kernel>
//   <kernel_addr>0xFFFF6004</kernel_addr>
TEST(EepromReadPlanGolden, Sh7055KlineMode4)
{
    InMemoryFileRepository repository;
    repository.files["kernels/ssmk_kline_sh7055.bin"] = {0xaa, 0xbb};
    LegacyFlashSnapshotAdapter adapter(repository);

    FileActions::EcuCalDefStructure ecu_cal_def;
    ecu_cal_def.McuType = "SH7055";
    ecu_cal_def.FlashMethod = "sub_ecu_eeprom_denso_sh7055_kline";
    ecu_cal_def.KernelStartAddr = "0xFFFF6004";

    auto plan = adapter.build_read_plan(ecu_cal_def, "sub_ecu_eeprom_denso_sh7055_kline",
                                        EepromReadMode::Mode4,
                                        "kernels/ssmk_kline_sh7055.bin");

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromKline);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);
    EXPECT_EQ(plan->target_id(), "sub_ecu_eeprom_denso_sh7055_kline");
    EXPECT_EQ(plan->mcu_name(), "SH7055");
    EXPECT_EQ(plan->kernel().load_address, 0xFFFF6004u);
}

} // namespace
} // namespace fastecu::flash

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
