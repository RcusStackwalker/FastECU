#include "src/backend/flash/eeprom/eeprom_read_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/in_memory_file_repository.h"

using ::testing::ElementsAre;

namespace fastecu::flash
{
namespace
{

// A K-Line EEPROM protocol reachable through a <car_model>. The real
// protocols.cfg has no such car_model for any K-Line EEPROM protocol (see
// the design doc's reachability table), so K-Line coverage requires a
// synthetic fixture -- that is a fixture-design constraint, not a reason to
// skip the family.
const char *kFixture = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
    <protocols>
        <protocol name="sub_ecu_eeprom_denso_sh7055_kline" alias="SH7055 EEPROM K-Line">
            <ecu>Denso SH7055</ecu>
            <mcu>SH7055</mcu>
            <kernel>ssmk_kline_sh7055.bin</kernel>
            <kernel_addr>0xFFFF6004</kernel_addr>
        </protocol>
        <protocol name="sub_ecu_eeprom_denso_sh7058_can" alias="SH7058 EEPROM CAN">
            <ecu>Denso SH7058</ecu>
            <mcu>SH7058</mcu>
            <kernel>ssmk_can_tp_sh7058.bin</kernel>
            <kernel_addr>0xFFFF3000</kernel_addr>
        </protocol>
        <protocol name="sub_ecu_eeprom_denso_sh7055_kline_cobb" alias="SH7055 EEPROM K-Line Cobb">
            <ecu>Denso SH7055</ecu>
            <mcu>SH7055</mcu>
            <kernel>ssmk_kline_sh7055.bin</kernel>
            <kernel_addr>0xFFFF6004</kernel_addr>
        </protocol>
        <protocol name="sub_ecu_eeprom_denso_sh7055_bad_kernel_addr" alias="SH7055 EEPROM bad kernel address">
            <ecu>Denso SH7055</ecu>
            <mcu>SH7055</mcu>
            <kernel>out_of_range.bin</kernel>
            <kernel_addr>0xFFFF0000</kernel_addr>
        </protocol>
        <protocol name="sub_ecu_eeprom_unreferenced" alias="Not in any car_model">
            <mcu>SH7058</mcu>
            <kernel>whatever.bin</kernel>
            <kernel_addr>0xFFFF3000</kernel_addr>
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
        <car_model>
            <make>Subaru</make><model>Impreza</model><version>WRX Cobb</version>
            <protocol>sub_ecu_eeprom_denso_sh7055_kline_cobb</protocol>
        </car_model>
        <car_model>
            <make>Subaru</make><model>Impreza</model><version>bad kernel address</version>
            <protocol>sub_ecu_eeprom_denso_sh7055_bad_kernel_addr</protocol>
        </car_model>
        <car_model>
            <make>Subaru</make><model>Missing</model><version>Protocol row</version>
            <protocol>sub_ecu_eeprom_denso_missing_protocol</protocol>
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

InMemoryFileRepository make_repository()
{
    InMemoryFileRepository repository;
    const std::string xml(kFixture);
    repository.files["protocols.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());
    repository.files["kernels/ssmk_kline_sh7055.bin"] = {0xaa, 0xbb};
    repository.files["kernels/ssmk_can_tp_sh7058.bin"] = {0x01, 0x02, 0x03};
    return repository;
}

InMemoryFileRepository make_repository_with_protocol(const std::string& name, const std::string& mcu,
                                                     const std::string& kernel, const std::string& kernel_addr)
{
    InMemoryFileRepository repository;
    const std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
    <protocols>
        <protocol name=")" +
        name + R"(" alias="Synthetic EEPROM protocol">
            <ecu>Denso EEPROM</ecu>
            <mcu>)" +
        mcu + R"(</mcu>
            <kernel>)" +
        kernel + R"(</kernel>
            <kernel_addr>)" +
        kernel_addr + R"(</kernel_addr>
        </protocol>
    </protocols>
    <car_models>
        <car_model>
            <make>Subaru</make><model>Synthetic</model><version>Test</version>
            <protocol>)" +
        name + R"(</protocol>
        </car_model>
    </car_models>
</config>)";
    repository.files["protocols.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());
    repository.files["kernels/" + kernel] = {0x01, 0x02, 0x03};
    return repository;
}

TEST(BuildEepromReadPlanTest, KlineProtocolProducesAKlinePlan)
{
    InMemoryFileRepository repository = make_repository();

    auto plan =
        build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7055_kline", EepromReadMode::Mode2, repository);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromKline);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);
    EXPECT_EQ(plan->mcu_name(), "SH7055");
    EXPECT_EQ(plan->target_id(), "sub_ecu_eeprom_denso_sh7055_kline");
    ASSERT_TRUE(plan->kernel().has_value());
    EXPECT_EQ(plan->kernel()->load_address, 0xFFFF6004U);
    EXPECT_THAT(plan->kernel()->bytes, ElementsAre(0xaa, 0xbb));
}

TEST(BuildEepromReadPlanTest, CanProtocolProducesACanPlan)
{
    InMemoryFileRepository repository = make_repository();

    auto plan =
        build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can", EepromReadMode::Mode3, repository);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromCan);
    EXPECT_EQ(plan->transport(), TransportKind::CanIso15765);
    EXPECT_EQ(plan->mcu_name(), "SH7058");
    ASSERT_TRUE(plan->kernel().has_value());
    EXPECT_EQ(plan->kernel()->load_address, 0xFFFF3000U);
}

// The kernel handle is directory + filename with NO separator inserted:
// kernel_files_directory already carries its trailing separator
// (config_paths.cpp:20 builds it as base + "/kernels/").
TEST(BuildEepromReadPlanTest, KernelHandleIsDirectoryPlusFilenameWithNoAddedSeparator)
{
    InMemoryFileRepository repository = make_repository();

    auto plan =
        build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can", EepromReadMode::Mode2, repository);

    ASSERT_TRUE(plan.has_value());
    // protocols.cfg is read by each catalog loader, then the kernel.
    ASSERT_EQ(repository.read_handles.size(), 3U);
    EXPECT_EQ(repository.read_handles.back(), "kernels/ssmk_can_tp_sh7058.bin");
}

// The deliberate divergence from LegacyCalibrationAdapter::bind_protocol,
// which substitutes a single-space placeholder here. A placeholder MCU or
// kernel address would build a plan that flashes garbage to an ECU.
TEST(BuildEepromReadPlanTest, ProtocolWithNoCarModelIsRejected)
{
    InMemoryFileRepository repository = make_repository();

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_unreferenced", EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(BuildEepromReadPlanTest, UnknownProtocolNameIsRejected)
{
    InMemoryFileRepository repository = make_repository();

    auto plan = build_eeprom_read_plan(test_paths(), "no_such_protocol", EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

// Every invalid configuration derivable from metadata is rejected before the
// kernel is read. The portable builder documents the same guarantee.
TEST(BuildEepromReadPlanTest, InvalidConfigIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();

    auto plan = build_eeprom_read_plan(test_paths(), "no_such_protocol", EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    // protocols.cfg was read; the kernel was not.
    EXPECT_EQ(repository.read_count("kernels/ssmk_can_tp_sh7058.bin"), 0);
}

TEST(BuildEepromReadPlanTest, InvalidModeIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();

    auto plan = build_eeprom_read_plan(
        test_paths(), "sub_ecu_eeprom_denso_sh7058_can",
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- exercising the invalid-value rejection path
        static_cast<EepromReadMode>(0), repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_count("kernels/ssmk_can_tp_sh7058.bin"), 0);
}

TEST(BuildEepromReadPlanTest, UnsupportedKlineSecurityIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7055_kline_cobb", EepromReadMode::Mode2,
                                       repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_count("kernels/ssmk_kline_sh7055.bin"), 0);
}

TEST(BuildEepromReadPlanTest, KernelAddressOutsideRamIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7055_bad_kernel_addr",
                                       EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_count("kernels/out_of_range.bin"), 0);
}

TEST(BuildEepromReadPlanTest, KernelAddressAtExclusiveRamEndIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository_with_protocol("sub_ecu_eeprom_denso_sh7058_can", "SH7058",
                                                                      "ssmk_can_tp_sh7058.bin", "0xFFFFC000");

    auto plan =
        build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can", EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_count("kernels/ssmk_can_tp_sh7058.bin"), 0);
}

TEST(BuildEepromReadPlanTest, CarModelReferencingAbsentProtocolIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_missing_protocol", EepromReadMode::Mode2,
                                       repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(plan.error().detail, "protocol 'sub_ecu_eeprom_denso_missing_protocol' is referenced by a car model "
                                   "but absent from the <protocols> section");
    EXPECT_EQ(repository.read_handles, (std::vector<std::string>{"protocols.cfg", "protocols.cfg"}));
}

TEST(BuildEepromReadPlanTest, KernelReadFailureIsPropagated)
{
    InMemoryFileRepository repository = make_repository();
    repository.read_errors["kernels/ssmk_can_tp_sh7058.bin"] = Error{ErrorKind::Internal, "disk error"};

    auto plan =
        build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can", EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Internal);
}

TEST(BuildEepromReadPlanTest, MissingProtocolsFileIsPropagated)
{
    InMemoryFileRepository repository; // no protocols.cfg at all

    auto plan =
        build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can", EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
}

// Successor to the deleted adapter's unknown-MCU rejection test. Removing
// resolve_sh705x_eeprom_region() from the metadata preflight must fail this.
TEST(BuildEepromReadPlanTest, UnknownMcuIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository_with_protocol(
        "sub_ecu_eeprom_denso_sh7058_can", "NOT_A_REAL_MCU", "ssmk_can_tp_sh7058.bin", "0xFFFF3000");

    auto plan =
        build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can", EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_count("kernels/ssmk_can_tp_sh7058.bin"), 0);
}

// Successor to the deleted adapter's malformed-address rejection test.
// Removing parse_kernel_start_addr() from metadata preflight must fail this.
TEST(BuildEepromReadPlanTest, UnparseableKernelAddrIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository =
        make_repository_with_protocol("sub_ecu_eeprom_denso_sh7058_can", "SH7058", "ssmk_can_tp_sh7058.bin", "not_hex");

    auto plan =
        build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can", EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_count("kernels/ssmk_can_tp_sh7058.bin"), 0);
}

// Successor to the deleted adapter's EcuTek suffix test. No shipped EEPROM
// protocol carries a security suffix today, so this uses a synthetic name.
// Removing the _ecutek branch from security_for_protocol() must fail this.
TEST(BuildEepromReadPlanTest, EcuTekSuffixProducesAnEcuTekPlan)
{
    InMemoryFileRepository repository = make_repository_with_protocol("sub_ecu_eeprom_denso_sh7058_can_ecutek",
                                                                      "SH7058", "ssmk_can_tp_sh7058.bin", "0xFFFF3000");

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can_ecutek", EepromReadMode::Mode2,
                                       repository);

    ASSERT_TRUE(plan.has_value());
    const auto *can_plan = std::get_if<DensoSh705xEepromCanPlan>(&plan->family_plan());
    ASSERT_NE(can_plan, nullptr);
    EXPECT_EQ(can_plan->security, DensoSecurityVariant::EcuTek);
}

TEST(BuildEepromReadPlanTest, EcuTekRaceRomAltSuffixIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository_with_protocol(
        "sub_ecu_eeprom_denso_sh7058_can_ecutek_racerom_alt", "SH7058", "ssmk_can_tp_sh7058.bin", "0xFFFF3000");

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can_ecutek_racerom_alt",
                                       EepromReadMode::Mode2, repository);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(repository.read_count("kernels/ssmk_can_tp_sh7058.bin"), 0);
}

TEST(BuildEepromReadPlanTest, EcuTekRaceRomSuffixStillProducesAnEcuTekRaceRomPlan)
{
    InMemoryFileRepository repository = make_repository_with_protocol("sub_ecu_eeprom_denso_sh7058_can_ecutek_racerom",
                                                                      "SH7058", "ssmk_can_tp_sh7058.bin", "0xFFFF3000");

    auto plan = build_eeprom_read_plan(test_paths(), "sub_ecu_eeprom_denso_sh7058_can_ecutek_racerom",
                                       EepromReadMode::Mode2, repository);

    ASSERT_TRUE(plan.has_value());
    const auto *can_plan = std::get_if<DensoSh705xEepromCanPlan>(&plan->family_plan());
    ASSERT_NE(can_plan, nullptr);
    EXPECT_EQ(can_plan->security, DensoSecurityVariant::EcuTekRaceRom);
}

} // namespace
} // namespace fastecu::flash
