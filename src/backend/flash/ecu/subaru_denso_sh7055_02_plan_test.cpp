#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

KernelImage TestKernel()
{
    return {.id = "k", .load_address = 0xffff6004, .bytes = {0xaa}};
}

FlashPlanFields ValidSh705502Fields(FlashOperation operation = FlashOperation::kRead)
{
    const int index = FindFlashDeviceIndex("SH7055");
    return {
        .operation = operation,
        .family = FlashFamily::kSubaruDensoSh705502,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_denso_sh7055_02",
        .mcu_name = "SH7055",
        .transfer_region = {kFlashDevices[index].fblocks[0].start, kFlashDevices[index].romsize},
        .erase_regions = {},
        .image = operation == FlashOperation::kRead
                     ? std::nullopt
                     : std::optional<bytes::Bytes>{bytes::Bytes(kFlashDevices[index].romsize, bytes::Byte{0})},
        .kernel = TestKernel(),
        .family_plan =
            SubaruDensoSh7055_02Plan{
                .tester_id = 0xf0,
                .target_id = 0x10,
                .read_ecu_id = operation == FlashOperation::kRead,
            },
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}},
    };
}

TEST(SubaruDensoSh7055_02Plan, BuildsBareReadAndWritePlansWithOperationSpecificEcuIdRead)
{
    auto read = BuildSubaruDensoSh705502Plan(FlashOperation::kRead, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
                                             TestKernel());
    ASSERT_THAT(read, fastecu::testing::IsOk());
    EXPECT_EQ(read->Family(), FlashFamily::kSubaruDensoSh705502);
    EXPECT_EQ(read->Transport(), TransportKind::kKline);
    const auto& read_family = std::get<SubaruDensoSh7055_02Plan>(read->FamilyPlan());
    EXPECT_EQ(read_family.tester_id, 0xf0);
    EXPECT_EQ(read_family.target_id, 0x10);
    EXPECT_TRUE(read_family.read_ecu_id);

    const int index = FindFlashDeviceIndex("SH7055");
    ASSERT_GE(index, 0);
    auto write = BuildSubaruDensoSh705502Plan(FlashOperation::kWrite, "sub_ecu_denso_sh7055_02", "SH7055",
                                              bytes::Bytes(kFlashDevices[index].romsize, bytes::Byte{0}), TestKernel());
    ASSERT_THAT(write, fastecu::testing::IsOk());
    EXPECT_FALSE(std::get<SubaruDensoSh7055_02Plan>(write->FamilyPlan()).read_ecu_id);
}

TEST(SubaruDensoSh7055_02Plan, AcceptsEcutekWithByteIdenticalWireParameters)
{
    auto bare = BuildSubaruDensoSh705502Plan(FlashOperation::kRead, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
                                             TestKernel());
    auto ecutek = BuildSubaruDensoSh705502Plan(FlashOperation::kRead, "sub_ecu_denso_sh7055_02_ecutek", "SH7055",
                                               std::nullopt, TestKernel());
    ASSERT_THAT(bare, fastecu::testing::IsOk());
    ASSERT_THAT(ecutek, fastecu::testing::IsOk());
    EXPECT_EQ(std::get<SubaruDensoSh7055_02Plan>(bare->FamilyPlan()).tester_id,
              std::get<SubaruDensoSh7055_02Plan>(ecutek->FamilyPlan()).tester_id);
    EXPECT_EQ(std::get<SubaruDensoSh7055_02Plan>(bare->FamilyPlan()).target_id,
              std::get<SubaruDensoSh7055_02Plan>(ecutek->FamilyPlan()).target_id);
    EXPECT_EQ(std::get<SubaruDensoSh7055_02Plan>(bare->FamilyPlan()).read_ecu_id,
              std::get<SubaruDensoSh7055_02Plan>(ecutek->FamilyPlan()).read_ecu_id);
}

TEST(SubaruDensoSh7055_02Plan, EveryAcceptedPlanRequiresCycleIgnitionConfirmation)
{
    const int index = FindFlashDeviceIndex("SH7055");
    ASSERT_GE(index, 0);
    for (const std::string_view protocol : {"sub_ecu_denso_sh7055_02", "sub_ecu_denso_sh7055_02_ecutek"})
    {
        for (const auto operation : {FlashOperation::kRead, FlashOperation::kWrite})
        {
            auto plan = BuildSubaruDensoSh705502Plan(
                operation, protocol, "SH7055",
                operation == FlashOperation::kRead
                    ? std::nullopt
                    : std::optional<bytes::Bytes>{bytes::Bytes(kFlashDevices[index].romsize, bytes::Byte{0})},
                TestKernel());
            ASSERT_THAT(plan, fastecu::testing::IsOk());
            ASSERT_EQ(plan->Confirmations().size(), 1U);
            EXPECT_EQ(plan->Confirmations().front().id, ConfirmationSpec::Id::kCycleIgnition);
        }
    }
}

TEST(SubaruDensoSh7055_02Plan, RejectsUnknownProtocol)
{
    ASSERT_THAT(BuildSubaruDensoSh705502Plan(FlashOperation::kRead, "sub_ecu_denso_sh7055_04", "SH7055", std::nullopt,
                                             TestKernel()),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoSh7055_02Plan, RejectsUnknownMcu)
{
    ASSERT_THAT(BuildSubaruDensoSh705502Plan(FlashOperation::kRead, "sub_ecu_denso_sh7055_02", "NOT_A_REAL_MCU",
                                             std::nullopt, TestKernel()),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoSh7055_02Plan, RejectsKnownButWrongMcu)
{
    for (const std::string_view mcu : {"SH7058", "MC68HC16Y5"})
    {
        ASSERT_THAT(BuildSubaruDensoSh705502Plan(FlashOperation::kRead, "sub_ecu_denso_sh7055_02", mcu, std::nullopt,
                                                 TestKernel()),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig))
            << mcu;
    }
}

TEST(SubaruDensoSh7055_02Plan, WriteAndTestWriteRequireExactRomSize)
{
    const int index = FindFlashDeviceIndex("SH7055");
    ASSERT_GE(index, 0);
    for (const auto operation : {FlashOperation::kWrite, FlashOperation::kTestWrite})
    {
        ASSERT_THAT(BuildSubaruDensoSh705502Plan(operation, "sub_ecu_denso_sh7055_02", "SH7055",
                                                 bytes::Bytes(kFlashDevices[index].romsize - 1, bytes::Byte{0}),
                                                 TestKernel()),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig));

        ASSERT_THAT(BuildSubaruDensoSh705502Plan(operation, "sub_ecu_denso_sh7055_02", "SH7055",
                                                 bytes::Bytes(kFlashDevices[index].romsize + 1, bytes::Byte{0}),
                                                 TestKernel()),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig));

        auto exact =
            BuildSubaruDensoSh705502Plan(operation, "sub_ecu_denso_sh7055_02", "SH7055",
                                         bytes::Bytes(kFlashDevices[index].romsize, bytes::Byte{0}), TestKernel());
        ASSERT_THAT(exact, fastecu::testing::IsOk());
        ASSERT_TRUE(exact->Image().has_value());
        EXPECT_EQ(exact->Image()->size(), kFlashDevices[index].romsize);
    }
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRejectsWrongTesterId)
{
    auto fields = ValidSh705502Fields();
    std::get<SubaruDensoSh7055_02Plan>(fields.family_plan).tester_id = 0xf1;
    auto plan = ValidateAndBuild(std::move(fields));
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ASSERT_THAT(ValidateSubaruDensoSh705502Plan(*plan), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRejectsWrongTargetId)
{
    auto fields = ValidSh705502Fields();
    std::get<SubaruDensoSh7055_02Plan>(fields.family_plan).target_id = 0x11;
    auto plan = ValidateAndBuild(std::move(fields));
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ASSERT_THAT(ValidateSubaruDensoSh705502Plan(*plan), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRequiresOperationSpecificEcuIdRead)
{
    for (const auto operation : {FlashOperation::kRead, FlashOperation::kTestWrite})
    {
        auto fields = ValidSh705502Fields(operation);
        auto& family = std::get<SubaruDensoSh7055_02Plan>(fields.family_plan);
        family.read_ecu_id = !family.read_ecu_id;
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_THAT(plan, fastecu::testing::IsOk());

        ASSERT_THAT(ValidateSubaruDensoSh705502Plan(*plan), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRejectsEraseRegions)
{
    auto fields = ValidSh705502Fields(FlashOperation::kTestWrite);
    fields.erase_regions.push_back({.start = 0, .length = 0x1000});
    auto plan = ValidateAndBuild(std::move(fields));
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ASSERT_THAT(ValidateSubaruDensoSh705502Plan(*plan), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRejectsWrongTransferRegion)
{
    const int index = FindFlashDeviceIndex("SH7055");
    ASSERT_GE(index, 0);
    for (const auto region : {MemoryRegion{kFlashDevices[index].fblocks[0].start + 1, kFlashDevices[index].romsize},
                              MemoryRegion{kFlashDevices[index].fblocks[0].start, kFlashDevices[index].romsize - 1}})
    {
        auto fields = ValidSh705502Fields();
        fields.transfer_region = region;
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_THAT(plan, fastecu::testing::IsOk());

        ASSERT_THAT(ValidateSubaruDensoSh705502Plan(*plan), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRequiresOnlyCycleIgnitionConfirmation)
{
    for (const auto& confirmations : {
             std::vector<ConfirmationSpec>{},
             std::vector<ConfirmationSpec>{ConfirmationSpec{.id = ConfirmationSpec::Id::kBeginEepromRead}},
             std::vector<ConfirmationSpec>{ConfirmationSpec{
                 .id = ConfirmationSpec::Id::kCycleIgnition,
                 .arguments = {{"unexpected", "argument"}},
             }},
         })
    {
        auto fields = ValidSh705502Fields();
        fields.confirmations = confirmations;
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_THAT(plan, fastecu::testing::IsOk());

        ASSERT_THAT(ValidateSubaruDensoSh705502Plan(*plan), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoSh7055_02Plan, KernelUploadAcceptsCanonicalAddressAndExactEnvelopeBoundary)
{
    constexpr std::uint32_t kKernelStart = 0xFFFF6004;
    for (KernelImage kernel : {
             KernelImage{.id = "lower", .load_address = kKernelStart, .bytes = {0x01}},
             KernelImage{.id = "full-envelope-fit", .load_address = kKernelStart, .bytes = bytes::Bytes(0x5ffc, 0)},
         })
    {
        ASSERT_THAT(BuildSubaruDensoSh705502Plan(FlashOperation::kRead, "sub_ecu_denso_sh7055_02", "SH7055",
                                                 std::nullopt, std::move(kernel)),
                    fastecu::testing::IsOk());
    }
}

TEST(SubaruDensoSh7055_02Plan, KernelUploadRejectsAddressAndPaddedFootprintOutsideModelRegion)
{
    constexpr std::uint32_t kKernelStart = 0xFFFF6004;
    for (KernelImage kernel : {
             KernelImage{.id = "below", .load_address = kKernelStart - 1, .bytes = {0x01}},
             KernelImage{.id = "shifted", .load_address = kKernelStart + 4, .bytes = {0x01}},
             KernelImage{.id = "padded-past-end", .load_address = kKernelStart, .bytes = bytes::Bytes(0x5ffd, 0)},
         })
    {
        ASSERT_THAT(BuildSubaruDensoSh705502Plan(FlashOperation::kRead, "sub_ecu_denso_sh7055_02", "SH7055",
                                                 std::nullopt, std::move(kernel)),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoSh7055_02Plan, KernelUploadRejectsLengthOutsideThreeByteWireField)
{
    bytes::Bytes too_large(0x00FFFFF9, bytes::Byte{0});
    auto plan = BuildSubaruDensoSh705502Plan(
        FlashOperation::kRead, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
        KernelImage{.id = "wire-overflow", .load_address = 0xFFFF6004, .bytes = std::move(too_large)});

    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_NE(plan.error().detail.find("24-bit"), std::string::npos);
}

} // namespace
} // namespace fastecu::flash
