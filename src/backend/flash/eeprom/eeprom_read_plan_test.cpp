#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/eeprom/eeprom_read_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/config/catalog.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

using ::testing::ElementsAre;

namespace fastecu::flash
{
namespace
{

constexpr config::ProtocolSpec kKline{.name = "sub_ecu_eeprom_denso_sh7055_kline",
                                      .mcu = "SH7055",
                                      .kernel = "ssmk_kline_sh7055.bin",
                                      .kernel_load_address = 0xFFFF6004U};
constexpr config::ProtocolSpec kCan{.name = "sub_ecu_eeprom_denso_sh7058_can",
                                    .mcu = "SH7058",
                                    .kernel = "ssmk_can_tp_sh7058.bin",
                                    .kernel_load_address = 0xFFFF3000U};
constexpr config::ProtocolSpec kKlineCobb{.name = "sub_ecu_eeprom_denso_sh7055_kline_cobb",
                                          .mcu = "SH7055",
                                          .kernel = "ssmk_kline_sh7055.bin",
                                          .kernel_load_address = 0xFFFF6004U};
constexpr config::ProtocolSpec kBadKernelAddress{.name = "sub_ecu_eeprom_denso_sh7055_bad_kernel_addr",
                                                 .mcu = "SH7055",
                                                 .kernel = "out_of_range.bin",
                                                 .kernel_load_address = 0xFFFF0000U};

config::ConfigPaths test_paths()
{
    config::ConfigPaths paths;
    paths.kernel_files_directory = "kernels/";
    return paths;
}

InMemoryFileRepository make_repository()
{
    InMemoryFileRepository repository;
    repository.files["kernels/ssmk_kline_sh7055.bin"] = {0xaa, 0xbb};
    repository.files["kernels/ssmk_can_tp_sh7058.bin"] = {0x01, 0x02, 0x03};
    return repository;
}

// A protocol like kCan with its own name, MCU and kernel address, and a
// three-byte kernel.
config::ProtocolSpec synthetic_protocol(std::string_view name, std::string_view mcu,
                                        std::optional<std::uint32_t> kernel_load_address)
{
    return {.name = name, .mcu = mcu, .kernel = "synthetic.bin", .kernel_load_address = kernel_load_address};
}

TEST(BuildEepromReadPlanTest, KlineProtocolProducesAKlinePlan)
{
    InMemoryFileRepository repository = make_repository();

    auto plan = build_eeprom_read_plan(test_paths(), kKline, EepromReadMode::Mode2, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
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

    auto plan = build_eeprom_read_plan(test_paths(), kCan, EepromReadMode::Mode3, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
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

    ASSERT_THAT(build_eeprom_read_plan(test_paths(), kCan, EepromReadMode::Mode2, repository),
                fastecu::testing::IsOk());
    // The kernel is the only file read.
    EXPECT_EQ(repository.read_handles, (std::vector<std::string>{"kernels/ssmk_can_tp_sh7058.bin"}));
}

TEST(BuildEepromReadPlanTest, InvalidModeIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();

    // Exercise the invalid-value rejection path.
    ASSERT_THAT(build_eeprom_read_plan(test_paths(), kCan,
                                       // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                                       static_cast<EepromReadMode>(0), repository),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(repository.read_count("kernels/ssmk_can_tp_sh7058.bin"), 0);
}

TEST(BuildEepromReadPlanTest, UnsupportedKlineSecurityIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();

    ASSERT_THAT(build_eeprom_read_plan(test_paths(), kKlineCobb, EepromReadMode::Mode2, repository),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(repository.read_count("kernels/ssmk_kline_sh7055.bin"), 0);
}

TEST(BuildEepromReadPlanTest, KernelAddressOutsideRamIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();

    ASSERT_THAT(build_eeprom_read_plan(test_paths(), kBadKernelAddress, EepromReadMode::Mode2, repository),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(repository.read_count("kernels/out_of_range.bin"), 0);
}

TEST(BuildEepromReadPlanTest, KernelAddressAtExclusiveRamEndIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    ASSERT_THAT(build_eeprom_read_plan(test_paths(),
                                       synthetic_protocol("sub_ecu_eeprom_denso_sh7058_can", "SH7058", 0xFFFFC000U),
                                       EepromReadMode::Mode2, repository),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(repository.read_count("kernels/synthetic.bin"), 0);
}

TEST(BuildEepromReadPlanTest, KernelReadFailureIsPropagated)
{
    InMemoryFileRepository repository = make_repository();
    repository.read_errors["kernels/ssmk_can_tp_sh7058.bin"] = Error{ErrorKind::Internal, "disk error"};

    ASSERT_THAT(build_eeprom_read_plan(test_paths(), kCan, EepromReadMode::Mode2, repository),
                fastecu::testing::IsErr(ErrorKind::Internal));
}

// Successor to the deleted adapter's unknown-MCU rejection test. Removing
// resolve_sh705x_eeprom_region() from the metadata preflight must fail this.
TEST(BuildEepromReadPlanTest, UnknownMcuIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    ASSERT_THAT(build_eeprom_read_plan(
                    test_paths(), synthetic_protocol("sub_ecu_eeprom_denso_sh7058_can", "NOT_A_REAL_MCU", 0xFFFF3000U),
                    EepromReadMode::Mode2, repository),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(repository.read_count("kernels/synthetic.bin"), 0);
}

// Successor to the deleted adapter's malformed-address rejection test: a
// protocol that declares no kernel load address cannot place the kernel.
TEST(BuildEepromReadPlanTest, MissingKernelLoadAddressIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};
    ASSERT_THAT(build_eeprom_read_plan(test_paths(),
                                       synthetic_protocol("sub_ecu_eeprom_denso_sh7058_can", "SH7058", std::nullopt),
                                       EepromReadMode::Mode2, repository),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(repository.read_count("kernels/synthetic.bin"), 0);
}

// Successor to the deleted adapter's EcuTek suffix test. No shipped EEPROM
// protocol carries a security suffix today, so this uses a synthetic name.
// Removing the _ecutek branch from security_for_protocol() must fail this.
TEST(BuildEepromReadPlanTest, EcuTekSuffixProducesAnEcuTekPlan)
{
    InMemoryFileRepository repository = make_repository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    auto plan = build_eeprom_read_plan(
        test_paths(), synthetic_protocol("sub_ecu_eeprom_denso_sh7058_can_ecutek", "SH7058", 0xFFFF3000U),
        EepromReadMode::Mode2, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto *can_plan = std::get_if<DensoSh705xEepromCanPlan>(&plan->family_plan());
    ASSERT_NE(can_plan, nullptr);
    EXPECT_EQ(can_plan->security, DensoSecurityVariant::EcuTek);
}

TEST(BuildEepromReadPlanTest, EcuTekRaceRomAltSuffixIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    ASSERT_THAT(build_eeprom_read_plan(
                    test_paths(),
                    synthetic_protocol("sub_ecu_eeprom_denso_sh7058_can_ecutek_racerom_alt", "SH7058", 0xFFFF3000U),
                    EepromReadMode::Mode2, repository),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(repository.read_count("kernels/synthetic.bin"), 0);
}

TEST(BuildEepromReadPlanTest, EcuTekRaceRomSuffixStillProducesAnEcuTekRaceRomPlan)
{
    InMemoryFileRepository repository = make_repository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    auto plan = build_eeprom_read_plan(
        test_paths(), synthetic_protocol("sub_ecu_eeprom_denso_sh7058_can_ecutek_racerom", "SH7058", 0xFFFF3000U),
        EepromReadMode::Mode2, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto *can_plan = std::get_if<DensoSh705xEepromCanPlan>(&plan->family_plan());
    ASSERT_NE(can_plan, nullptr);
    EXPECT_EQ(can_plan->security, DensoSecurityVariant::EcuTekRaceRom);
}

} // namespace
} // namespace fastecu::flash
