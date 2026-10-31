#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "src/backend/config/builtin_catalog.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using ::testing::_;
using ::testing::Each;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Optional;

constexpr std::string_view kProtocol = "sub_ecu_denso_mc68hc16y5_02_bdm";
constexpr std::string_view kMcu = "MC68HC16Y5";

KernelImage Kernel(bytes::Bytes content, std::uint32_t load_address = 0x20000)
{
    return KernelImage{.id = "bdm-kernel", .load_address = load_address, .bytes = std::move(content)};
}

FlashPlanFields ReadFields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
        .target_id = std::string(kProtocol),
        .mcu_name = std::string(kMcu),
        .transfer_region = MemoryRegion{0, 0x30000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = 115200},
        .confirmations = {},
    };
}

FlashPlanFields WriteFields(std::uint32_t size)
{
    FlashPlanFields fields = ReadFields();
    fields.operation = FlashOperation::kWrite;
    fields.transfer_region = MemoryRegion{0x20000, size};
    fields.image = bytes::Bytes(size, 0xab);
    fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kKernelBootstrap}};
    return fields;
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ReadCoversTheAddressSpaceImage)
{
    const auto plan =
        BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt, std::nullopt);
    ASSERT_THAT(plan, IsOk());
    EXPECT_EQ(plan->Family(), FlashFamily::kSubaruDensoMc68hc16y502Bdm);
    EXPECT_EQ(plan->Transport(), TransportKind::kKline);
    EXPECT_EQ(plan->TransferRegion(), (MemoryRegion{0, 0x30000}));
    EXPECT_THAT(plan->Image(), Eq(std::nullopt));
    EXPECT_THAT(plan->Kernel(), Eq(std::nullopt));
    EXPECT_EQ(plan->FamilyPlan().Get<SubaruDensoMc68hc16y5_02BdmPlan>().baud, 115200);
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WritePadsTheKernelAndCarriesItAsTheImage)
{
    const auto plan = BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, kProtocol, kMcu, std::nullopt,
                                                          Kernel(bytes::Bytes(33, 0xab)));
    ASSERT_THAT(plan, IsOk());
    ASSERT_THAT(plan->Image(), Optional(_));
    const bytes::Bytes& image = *plan->Image();
    ASSERT_EQ(image.size(), 64U);
    EXPECT_THAT(bytes::ByteView(image).first(33), Each(0xab));
    EXPECT_THAT(bytes::ByteView(image).subspan(33), Each(0x00));
    EXPECT_EQ(plan->TransferRegion(), (MemoryRegion{0x20000, 64}));
    EXPECT_THAT(plan->Kernel(), Eq(std::nullopt));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WriteAcceptsAKernelThatFillsTheRamBlockExactly)
{
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, kProtocol, kMcu, std::nullopt,
                                                    Kernel(bytes::Bytes(0x8000, 0x01))),
                IsOk());
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, kProtocol, kMcu, std::nullopt,
                                                    Kernel(bytes::Bytes(0x8001, 0x01))),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsEveryOtherIdentity)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_denso_mc68hc16y5_02_bdm", "MC68HC16Y5_TPU"},
             {"sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5"},
             {"sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5_TPU"},
             {"sub_ecu_denso_mc68hc16y5_02_bdm_x", "MC68HC16Y5"},
         }))
    {
        SCOPED_TRACE(::testing::PrintToString(protocol));
        EXPECT_THAT(
            BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kRead, protocol, mcu, std::nullopt, std::nullopt),
            IsErr(ErrorKind::kInvalidConfig))
            << protocol << " / " << mcu;
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsTestWrite)
{
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kTestWrite, kProtocol, kMcu, std::nullopt,
                                                    Kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::kUnsupported));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsARomImageForEveryOperation)
{
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kRead, kProtocol, kMcu, bytes::Bytes(0x30000, 0x00),
                                                    std::nullopt),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, kProtocol, kMcu,
                                                    bytes::Bytes(0x30000, 0x00), Kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ReadRejectsAKernel)
{
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt,
                                                    Kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WriteRejectsAMissingEmptyOrMisplacedKernel)
{
    EXPECT_THAT(
        BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, kProtocol, kMcu, std::nullopt, std::nullopt),
        IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, kProtocol, kMcu, std::nullopt,
                                                    Kernel(bytes::Bytes{})),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, kProtocol, kMcu, std::nullopt,
                                                    Kernel(bytes::Bytes(0x20, 0x01), 0x21000)),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ValidatorRejectsHandBuiltShapes)
{
    struct Case
    {
        const char *name;
        FlashPlanFields fields;
        ErrorKind expected;
    };
    std::vector<Case> cases;
    {
        auto fields = ReadFields();
        fields.family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = 9600};
        cases.push_back({"wrong baud", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = ReadFields();
        fields.kernel = Kernel(bytes::Bytes(0x20, 0x01));
        cases.push_back({"read with kernel", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = ReadFields();
        fields.transfer_region = MemoryRegion{0, 0x28000};
        cases.push_back({"packed read region", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields(0x20);
        fields.confirmations = {};
        cases.push_back({"write without consent", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields(0x20);
        fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kStartKlineRead}};
        cases.push_back({"write with another id", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields(0x20);
        fields.confirmations = {
            ConfirmationSpec{.id = ConfirmationSpec::Id::kKernelBootstrap, .arguments = {{"unexpected", "argument"}}}};
        cases.push_back({"consent with arguments", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields(0x20);
        fields.confirmations.push_back(ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition});
        cases.push_back({"write with an extra consent", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = ReadFields();
        fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kKernelBootstrap}};
        cases.push_back({"read with the bootstrap consent", std::move(fields), ErrorKind::kInvalidConfig});
    }
    cases.push_back({"unaligned image", WriteFields(0x21), ErrorKind::kInvalidConfig});
    cases.push_back({"oversize image", WriteFields(0x8020), ErrorKind::kInvalidConfig});
    {
        auto fields = WriteFields(0x20);
        fields.transfer_region = MemoryRegion{0x20000, 0x40};
        cases.push_back({"region/image mismatch", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields(0x20);
        fields.transfer_region = MemoryRegion{0, 0x20};
        cases.push_back({"image outside RAM", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields(0x20);
        fields.operation = FlashOperation::kTestWrite;
        cases.push_back({"test write", std::move(fields), ErrorKind::kUnsupported});
    }

    for (auto& test_case : cases)
    {
        SCOPED_TRACE(::testing::PrintToString(test_case));
        auto plan = ValidateAndBuild(std::move(test_case.fields));
        ASSERT_THAT(plan, IsOk()) << test_case.name;
        EXPECT_THAT(ValidateSubaruDensoMc68hc16y502BdmPlan(*plan), IsErr(test_case.expected)) << test_case.name;
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WriteCarriesTheKernelBootstrapConsentAndReadNone)
{
    const auto read =
        BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kRead, kProtocol, kMcu, std::nullopt, std::nullopt);
    ASSERT_THAT(read, IsOk());
    EXPECT_THAT(read->Confirmations(), IsEmpty());

    const auto write = BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, kProtocol, kMcu, std::nullopt,
                                                           Kernel(bytes::Bytes(0x20, 0x01)));
    ASSERT_THAT(write, IsOk());
    ASSERT_EQ(write->Confirmations().size(), 1U);
    EXPECT_EQ(write->Confirmations().front().id, ConfirmationSpec::Id::kKernelBootstrap);
    EXPECT_THAT(write->Confirmations().front().arguments, IsEmpty());
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ValidatorAcceptsTheBuilderShapes)
{
    for (auto fields : {ReadFields(), WriteFields(0x20)})
    {
        SCOPED_TRACE(::testing::PrintToString(fields));
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_THAT(plan, IsOk());
        EXPECT_THAT(ValidateSubaruDensoMc68hc16y502BdmPlan(*plan), IsOk());
    }
}
// Only Write reads the kernel file, and the operator's ROM file never reaches
// the plan: BDM never writes the ROM.
TEST(SubaruDensoMc68hc16y502BdmPrepare, AReadReadsNoFileAndDropsTheRomFile)
{
    const config::ProtocolSpec& protocol = *config::BuiltinCatalog().FindProtocol("sub_ecu_denso_mc68hc16y5_02_bdm");
    InMemoryFileRepository files;

    const auto plan =
        PrepareSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kRead, protocol, {}, bytes::Bytes(0x30000, 0x00), files);

    ASSERT_THAT(plan, IsOk());
    EXPECT_EQ(plan->Image(), std::nullopt);
    EXPECT_EQ(plan->Kernel(), std::nullopt);
    EXPECT_THAT(files.read_handles, IsEmpty());
}

TEST(SubaruDensoMc68hc16y502BdmPrepare, AWriteUploadsTheProtocolsKernelFileNotTheRomFile)
{
    const config::ProtocolSpec& protocol = *config::BuiltinCatalog().FindProtocol("sub_ecu_denso_mc68hc16y5_02_bdm");
    const config::ConfigPaths paths{.kernel_files_directory = "kernels/"};
    InMemoryFileRepository files;
    files.files[config::KernelFileHandle(paths, protocol.kernel)] = {0x01, 0x02};

    const auto plan = PrepareSubaruDensoMc68hc16y502BdmPlan(FlashOperation::kWrite, protocol, paths,
                                                            bytes::Bytes(0x30000, 0x00), files);

    // The write image is the chunk-padded kernel file, not the ROM file.
    bytes::Bytes padded_kernel(kSubaruDensoMc68hc16y5_02BdmUploadChunk, 0x00);
    padded_kernel[0] = 0x01;
    padded_kernel[1] = 0x02;
    ASSERT_THAT(plan, IsOk());
    EXPECT_EQ(plan->Image(), padded_kernel);
    EXPECT_THAT(files.read_handles, ::testing::ElementsAre(config::KernelFileHandle(paths, protocol.kernel)));
}

} // namespace
} // namespace fastecu::flash
