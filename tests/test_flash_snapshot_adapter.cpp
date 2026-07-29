// tests/test_flash_snapshot_adapter.cpp
#include "src/platform/desktop/common/flash/flash_snapshot_adapter.h"

#include <gtest/gtest.h>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

namespace fastecu::flash
{
namespace
{

TEST(LegacyFlashSnapshotAdapterTest, ValidSnapshotProducesAKlinePlan)
{
    InMemoryFileRepository repository;
    repository.files["kernel-handle-1"] = {0xaa, 0xbb};
    LegacyFlashSnapshotAdapter adapter(repository);

    // KernelStartAddr is a "0x"-prefixed hex string in real config, e.g.
    // resources/shared/config/protocols.cfg:922
    // <kernel_addr>0xFFFF6004</kernel_addr> for sub_ecu_eeprom_denso_sh7055_kline,
    // parsed via QString::toUInt(&ok, 16) everywhere in the legacy code (e.g.
    // src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp:78).
    FileActions::EcuCalDefStructure ecu_cal_def;
    ecu_cal_def.McuType = "SH7055";
    ecu_cal_def.FlashMethod = "sub_ecu_eeprom_denso_sh7055_kline";
    ecu_cal_def.KernelStartAddr = "0xFFFF6004";

    auto plan = adapter.build_read_plan(ecu_cal_def, "sub_ecu_eeprom_denso_sh7055_kline",
                                        EepromReadMode::Mode2, "kernel-handle-1");

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromKline);
    EXPECT_EQ(repository.read_handles, std::vector<std::string>{"kernel-handle-1"});
}

TEST(LegacyFlashSnapshotAdapterTest, FileRepositoryReadFailureIsPropagated)
{
    InMemoryFileRepository repository;
    repository.next_read_result = fail(ErrorKind::Internal, "disk error");
    LegacyFlashSnapshotAdapter adapter(repository);

    FileActions::EcuCalDefStructure ecu_cal_def;
    ecu_cal_def.McuType = "SH7055";
    ecu_cal_def.FlashMethod = "sub_ecu_eeprom_denso_sh7055_kline";
    ecu_cal_def.KernelStartAddr = "0xFFFF6004";

    auto plan = adapter.build_read_plan(ecu_cal_def, "sub_ecu_eeprom_denso_sh7055_kline",
                                        EepromReadMode::Mode2, "missing-handle");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Internal);
    EXPECT_EQ(repository.read_handles, std::vector<std::string>{"missing-handle"});
}

TEST(LegacyFlashSnapshotAdapterTest, UnknownMcuTypeIsRejectedBeforeAnyFileRead)
{
    InMemoryFileRepository repository;
    LegacyFlashSnapshotAdapter adapter(repository);

    FileActions::EcuCalDefStructure ecu_cal_def;
    ecu_cal_def.McuType = "NOT_A_REAL_MCU";
    ecu_cal_def.FlashMethod = "sub_ecu_eeprom_denso_sh7055_kline";
    ecu_cal_def.KernelStartAddr = "0xFFFF6004";

    auto plan = adapter.build_read_plan(ecu_cal_def, "sub_ecu_eeprom_denso_sh7055_kline",
                                        EepromReadMode::Mode2, "kernel-handle-1");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(repository.read_handles.empty()); // no I/O before rejection
}

TEST(LegacyFlashSnapshotAdapterTest, UnparseableKernelStartAddrIsRejectedBeforeAnyFileRead)
{
    // Real-world guard against a malformed or missing protocols.cfg entry --
    // QString::toUInt(&ok, 16) sets ok=false rather than throwing.
    InMemoryFileRepository repository;
    LegacyFlashSnapshotAdapter adapter(repository);

    FileActions::EcuCalDefStructure ecu_cal_def;
    ecu_cal_def.McuType = "SH7055";
    ecu_cal_def.FlashMethod = "sub_ecu_eeprom_denso_sh7055_kline";
    ecu_cal_def.KernelStartAddr = "not-a-hex-address";

    auto plan = adapter.build_read_plan(ecu_cal_def, "sub_ecu_eeprom_denso_sh7055_kline",
                                        EepromReadMode::Mode2, "kernel-handle-1");

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(repository.read_handles.empty());
}

TEST(LegacyFlashSnapshotAdapterTest, EcuTekSecuritySuffixOnFlashMethodProducesEcuTekPlan)
{
    // Confirmed real fact (not the brief's guess): the security-variant
    // suffix lives directly on FlashMethod, because mainwindow.cpp copies
    // configValues->flash_protocol_selected_protocol_name -- which already
    // carries the "_ecutek"/"_cobb"/"_ecutek_racerom"/"_ecutek_racerom_alt"
    // suffix straight from the protocol's XML `name` attribute in
    // resources/shared/config/protocols.cfg (e.g. line 223
    // "sub_ecu_denso_sh7055_02_ecutek") -- verbatim into
    // ecuCalDef[rom_number]->FlashMethod (mainwindow.cpp:1119, 1145). There is
    // no separate EcuCalDefStructure field for it.
    InMemoryFileRepository repository;
    repository.files["kernel-handle-1"] = {0xaa, 0xbb};
    LegacyFlashSnapshotAdapter adapter(repository);

    FileActions::EcuCalDefStructure ecu_cal_def;
    ecu_cal_def.McuType = "SH7055";
    ecu_cal_def.FlashMethod = "sub_ecu_eeprom_denso_sh7055_kline_ecutek";
    ecu_cal_def.KernelStartAddr = "0xFFFF6004";

    auto plan = adapter.build_read_plan(ecu_cal_def, "sub_ecu_eeprom_denso_sh7055_kline",
                                        EepromReadMode::Mode2, "kernel-handle-1");

    ASSERT_TRUE(plan.has_value());
    ASSERT_TRUE(std::holds_alternative<DensoSh705xEepromKlinePlan>(plan->family_plan()));
    EXPECT_EQ(std::get<DensoSh705xEepromKlinePlan>(plan->family_plan()).security,
              DensoSecurityVariant::EcuTek);
}

TEST(LegacyFlashSnapshotAdapterTest, CanProtocolNameProducesACanPlan)
{
    InMemoryFileRepository repository;
    repository.files["kernel-handle-1"] = {0xaa, 0xbb};
    LegacyFlashSnapshotAdapter adapter(repository);

    FileActions::EcuCalDefStructure ecu_cal_def;
    ecu_cal_def.McuType = "SH7058";
    ecu_cal_def.FlashMethod = "sub_ecu_eeprom_denso_sh7058_densocan";
    ecu_cal_def.KernelStartAddr = "0xFFFF3000";

    auto plan = adapter.build_read_plan(ecu_cal_def, "sub_ecu_eeprom_denso_sh7058_densocan",
                                        EepromReadMode::Mode2, "kernel-handle-1");

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromCan);
}

} // namespace
} // namespace fastecu::flash

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
