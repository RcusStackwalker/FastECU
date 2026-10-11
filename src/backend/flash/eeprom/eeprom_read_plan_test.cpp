#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/eeprom/eeprom_read_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/config/builtin_catalog.h"
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

config::ConfigPaths TestPaths()
{
    config::ConfigPaths paths;
    paths.kernel_files_directory = "kernels/";
    return paths;
}

InMemoryFileRepository MakeRepository()
{
    InMemoryFileRepository repository;
    repository.files["kernels/ssmk_kline_sh7055.bin"] = {0xaa, 0xbb};
    repository.files["kernels/ssmk_can_tp_sh7058.bin"] = {0x01, 0x02, 0x03};
    return repository;
}

// A protocol like kCan with its own name, MCU and kernel address, and a
// three-byte kernel.
config::ProtocolSpec SyntheticProtocol(std::string_view name, std::string_view mcu,
                                       std::optional<std::uint32_t> kernel_load_address)
{
    return {.name = name, .mcu = mcu, .kernel = "synthetic.bin", .kernel_load_address = kernel_load_address};
}

TEST(BuildEepromReadPlanTest, KlineProtocolProducesAKlinePlan)
{
    InMemoryFileRepository repository = MakeRepository();

    auto plan = BuildEepromReadPlan(TestPaths(), kKline, EepromReadMode::kMode2, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->Family(), FlashFamily::kDensoSh705xEepromKline);
    EXPECT_EQ(plan->Transport(), TransportKind::kKline);
    EXPECT_EQ(plan->McuName(), "SH7055");
    EXPECT_EQ(plan->TargetId(), "sub_ecu_eeprom_denso_sh7055_kline");
    ASSERT_TRUE(plan->Kernel().has_value());
    EXPECT_EQ(plan->Kernel()->load_address, 0xFFFF6004U);
    EXPECT_THAT(plan->Kernel()->bytes, ElementsAre(0xaa, 0xbb));
}

TEST(BuildEepromReadPlanTest, CanProtocolProducesACanPlan)
{
    InMemoryFileRepository repository = MakeRepository();

    auto plan = BuildEepromReadPlan(TestPaths(), kCan, EepromReadMode::kMode3, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->Family(), FlashFamily::kDensoSh705xEepromCan);
    EXPECT_EQ(plan->Transport(), TransportKind::kCanIso15765);
    EXPECT_EQ(plan->McuName(), "SH7058");
    ASSERT_TRUE(plan->Kernel().has_value());
    EXPECT_EQ(plan->Kernel()->load_address, 0xFFFF3000U);
}

// The kernel handle is directory + filename with NO separator inserted:
// kernel_files_directory already carries its trailing separator
// (config_paths.cpp:20 builds it as base + "/kernels/").
TEST(BuildEepromReadPlanTest, KernelHandleIsDirectoryPlusFilenameWithNoAddedSeparator)
{
    InMemoryFileRepository repository = MakeRepository();

    ASSERT_THAT(BuildEepromReadPlan(TestPaths(), kCan, EepromReadMode::kMode2, repository), fastecu::testing::IsOk());
    // The kernel is the only file read.
    EXPECT_EQ(repository.read_handles, (std::vector<std::string>{"kernels/ssmk_can_tp_sh7058.bin"}));
}

TEST(BuildEepromReadPlanTest, InvalidModeIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = MakeRepository();

    // Exercise the invalid-value rejection path.
    ASSERT_THAT(BuildEepromReadPlan(TestPaths(), kCan,
                                    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                                    static_cast<EepromReadMode>(0), repository),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(repository.ReadCount("kernels/ssmk_can_tp_sh7058.bin"), 0);
}

TEST(BuildEepromReadPlanTest, UnsupportedKlineSecurityIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = MakeRepository();

    ASSERT_THAT(BuildEepromReadPlan(TestPaths(), kKlineCobb, EepromReadMode::kMode2, repository),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(repository.ReadCount("kernels/ssmk_kline_sh7055.bin"), 0);
}

TEST(BuildEepromReadPlanTest, KernelAddressOutsideRamIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = MakeRepository();

    ASSERT_THAT(BuildEepromReadPlan(TestPaths(), kBadKernelAddress, EepromReadMode::kMode2, repository),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(repository.ReadCount("kernels/out_of_range.bin"), 0);
}

TEST(BuildEepromReadPlanTest, KernelAddressAtExclusiveRamEndIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = MakeRepository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    ASSERT_THAT(BuildEepromReadPlan(TestPaths(),
                                    SyntheticProtocol("sub_ecu_eeprom_denso_sh7058_can", "SH7058", 0xFFFFC000U),
                                    EepromReadMode::kMode2, repository),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(repository.ReadCount("kernels/synthetic.bin"), 0);
}

TEST(BuildEepromReadPlanTest, KernelReadFailureIsPropagated)
{
    InMemoryFileRepository repository = MakeRepository();
    repository.read_errors["kernels/ssmk_can_tp_sh7058.bin"] = Error{ErrorKind::kInternal, "disk error"};

    ASSERT_THAT(BuildEepromReadPlan(TestPaths(), kCan, EepromReadMode::kMode2, repository),
                fastecu::testing::IsErr(ErrorKind::kInternal));
}

// Successor to the deleted adapter's unknown-MCU rejection test. Removing
// resolve_sh705x_eeprom_region() from the metadata preflight must fail this.
TEST(BuildEepromReadPlanTest, UnknownMcuIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = MakeRepository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    ASSERT_THAT(BuildEepromReadPlan(TestPaths(),
                                    SyntheticProtocol("sub_ecu_eeprom_denso_sh7058_can", "NOT_A_REAL_MCU", 0xFFFF3000U),
                                    EepromReadMode::kMode2, repository),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(repository.ReadCount("kernels/synthetic.bin"), 0);
}

// Successor to the deleted adapter's malformed-address rejection test: a
// protocol that declares no kernel load address cannot place the kernel.
TEST(BuildEepromReadPlanTest, MissingKernelLoadAddressIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = MakeRepository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};
    ASSERT_THAT(BuildEepromReadPlan(TestPaths(),
                                    SyntheticProtocol("sub_ecu_eeprom_denso_sh7058_can", "SH7058", std::nullopt),
                                    EepromReadMode::kMode2, repository),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(repository.ReadCount("kernels/synthetic.bin"), 0);
}

// Successor to the deleted adapter's EcuTek suffix test. No shipped EEPROM
// protocol carries a security suffix today, so this uses a synthetic name.
// Removing the _ecutek branch from security_for_protocol() must fail this.
TEST(BuildEepromReadPlanTest, EcuTekSuffixProducesAnEcuTekPlan)
{
    InMemoryFileRepository repository = MakeRepository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    auto plan = BuildEepromReadPlan(TestPaths(),
                                    SyntheticProtocol("sub_ecu_eeprom_denso_sh7058_can_ecutek", "SH7058", 0xFFFF3000U),
                                    EepromReadMode::kMode2, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto *can_plan = std::get_if<DensoSh705xEepromCanPlan>(&plan->FamilyPlan());
    ASSERT_NE(can_plan, nullptr);
    EXPECT_EQ(can_plan->security, DensoSecurityVariant::kEcuTek);
}

TEST(BuildEepromReadPlanTest, EcuTekRaceRomAltSuffixIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = MakeRepository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    ASSERT_THAT(BuildEepromReadPlan(
                    TestPaths(),
                    SyntheticProtocol("sub_ecu_eeprom_denso_sh7058_can_ecutek_racerom_alt", "SH7058", 0xFFFF3000U),
                    EepromReadMode::kMode2, repository),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(repository.ReadCount("kernels/synthetic.bin"), 0);
}

TEST(BuildEepromReadPlanTest, EcuTekRaceRomSuffixStillProducesAnEcuTekRaceRomPlan)
{
    InMemoryFileRepository repository = MakeRepository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};

    auto plan = BuildEepromReadPlan(
        TestPaths(), SyntheticProtocol("sub_ecu_eeprom_denso_sh7058_can_ecutek_racerom", "SH7058", 0xFFFF3000U),
        EepromReadMode::kMode2, repository);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto *can_plan = std::get_if<DensoSh705xEepromCanPlan>(&plan->FamilyPlan());
    ASSERT_NE(can_plan, nullptr);
    EXPECT_EQ(can_plan->security, DensoSecurityVariant::kEcuTekRaceRom);
}

// The bundled kernel file named `name`, from $(locations //resources/shared:kernel_files).
std::vector<std::uint8_t> BundledKernel(std::string_view name)
{
    const char *paths = std::getenv("KERNEL_FILES");
    std::istringstream stream{paths == nullptr ? "" : paths};
    for (std::string path; stream >> path;)
    {
        if (path.ends_with("/" + std::string(name)))
        {
            std::ifstream file{path, std::ios::binary};
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }
    }
    return {};
}

TEST(BuildEepromReadPlanTest, EveryBuiltinEepromProtocolBuildsWithItsBundledKernel)
{
    int checked = 0;
    for (const config::ProtocolSpec& protocol : config::BuiltinCatalog().Protocols())
    {
        if (!protocol.name.starts_with("sub_ecu_eeprom_"))
        {
            continue;
        }
        InMemoryFileRepository repository;
        repository.files["kernels/" + std::string(protocol.kernel)] = BundledKernel(protocol.kernel);
        ASSERT_FALSE(repository.files.begin()->second.empty()) << protocol.kernel;
        for (EepromReadMode mode : {EepromReadMode::kMode2, EepromReadMode::kMode3, EepromReadMode::kMode4})
        {
            EXPECT_THAT(BuildEepromReadPlan(TestPaths(), protocol, mode, repository), fastecu::testing::IsOk())
                << protocol.name << " mode " << static_cast<int>(mode);
        }
        ++checked;
    }
    EXPECT_EQ(checked, 6);
}

} // namespace
} // namespace fastecu::flash
