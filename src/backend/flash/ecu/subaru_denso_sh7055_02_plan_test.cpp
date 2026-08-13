#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

KernelImage test_kernel()
{
    return {.id = "k", .load_address = 0xffff6004, .bytes = {0xaa}};
}

FlashPlanFields valid_sh7055_02_fields(FlashOperation operation = FlashOperation::Read)
{
    const int index = find_flash_device_index("SH7055");
    return {
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh7055_02,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_denso_sh7055_02",
        .mcu_name = "SH7055",
        .transfer_region = {flashdevices[index].fblocks[0].start, flashdevices[index].romsize},
        .erase_regions = {},
        .image = operation == FlashOperation::Read
                     ? std::nullopt
                     : std::optional<bytes::Bytes>{bytes::Bytes(flashdevices[index].romsize, bytes::Byte{0})},
        .kernel = test_kernel(),
        .family_plan = SubaruDensoSh7055_02Plan{
            .tester_id = 0xf0,
            .target_id = 0x10,
            .read_ecu_id = operation == FlashOperation::Read,
        },
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}},
    };
}

TEST(SubaruDensoSh7055_02Plan, BuildsBareReadAndWritePlansWithOperationSpecificEcuIdRead)
{
    auto read = build_subaru_denso_sh7055_02_plan(
        FlashOperation::Read, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt, test_kernel());
    ASSERT_TRUE(read.has_value()) << read.error().detail;
    EXPECT_EQ(read->family(), FlashFamily::SubaruDensoSh7055_02);
    EXPECT_EQ(read->transport(), TransportKind::Kline);
    const auto& read_family = std::get<SubaruDensoSh7055_02Plan>(read->family_plan());
    EXPECT_EQ(read_family.tester_id, 0xf0);
    EXPECT_EQ(read_family.target_id, 0x10);
    EXPECT_TRUE(read_family.read_ecu_id);

    const int index = find_flash_device_index("SH7055");
    ASSERT_GE(index, 0);
    auto write = build_subaru_denso_sh7055_02_plan(
        FlashOperation::Write, "sub_ecu_denso_sh7055_02", "SH7055",
        bytes::Bytes(flashdevices[index].romsize, bytes::Byte{0}), test_kernel());
    ASSERT_TRUE(write.has_value()) << write.error().detail;
    EXPECT_FALSE(std::get<SubaruDensoSh7055_02Plan>(write->family_plan()).read_ecu_id);
}

TEST(SubaruDensoSh7055_02Plan, AcceptsEcutekWithByteIdenticalWireParameters)
{
    auto bare = build_subaru_denso_sh7055_02_plan(
        FlashOperation::Read, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt, test_kernel());
    auto ecutek = build_subaru_denso_sh7055_02_plan(
        FlashOperation::Read, "sub_ecu_denso_sh7055_02_ecutek", "SH7055", std::nullopt, test_kernel());
    ASSERT_TRUE(bare.has_value()) << bare.error().detail;
    ASSERT_TRUE(ecutek.has_value()) << ecutek.error().detail;
    EXPECT_EQ(std::get<SubaruDensoSh7055_02Plan>(bare->family_plan()).tester_id,
              std::get<SubaruDensoSh7055_02Plan>(ecutek->family_plan()).tester_id);
    EXPECT_EQ(std::get<SubaruDensoSh7055_02Plan>(bare->family_plan()).target_id,
              std::get<SubaruDensoSh7055_02Plan>(ecutek->family_plan()).target_id);
    EXPECT_EQ(std::get<SubaruDensoSh7055_02Plan>(bare->family_plan()).read_ecu_id,
              std::get<SubaruDensoSh7055_02Plan>(ecutek->family_plan()).read_ecu_id);
}

TEST(SubaruDensoSh7055_02Plan, EveryAcceptedPlanRequiresCycleIgnitionConfirmation)
{
    const int index = find_flash_device_index("SH7055");
    ASSERT_GE(index, 0);
    for (const std::string_view protocol : {"sub_ecu_denso_sh7055_02",
                                            "sub_ecu_denso_sh7055_02_ecutek"})
    {
        for (const auto operation : {FlashOperation::Read, FlashOperation::Write})
        {
            auto plan = build_subaru_denso_sh7055_02_plan(
                operation, protocol, "SH7055",
                operation == FlashOperation::Read
                    ? std::nullopt
                    : std::optional<bytes::Bytes>{
                          bytes::Bytes(flashdevices[index].romsize, bytes::Byte{0})},
                test_kernel());
            ASSERT_TRUE(plan.has_value()) << plan.error().detail;
            ASSERT_EQ(plan->confirmations().size(), 1);
            EXPECT_EQ(plan->confirmations().front().id, ConfirmationSpec::Id::CycleIgnition);
        }
    }
}

TEST(SubaruDensoSh7055_02Plan, RejectsUnknownProtocol)
{
    auto plan = build_subaru_denso_sh7055_02_plan(
        FlashOperation::Read, "sub_ecu_denso_sh7055_04", "SH7055", std::nullopt, test_kernel());
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruDensoSh7055_02Plan, RejectsUnknownMcu)
{
    auto plan = build_subaru_denso_sh7055_02_plan(
        FlashOperation::Read, "sub_ecu_denso_sh7055_02", "NOT_A_REAL_MCU", std::nullopt, test_kernel());
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruDensoSh7055_02Plan, WriteAndTestWriteRequireExactRomSize)
{
    const int index = find_flash_device_index("SH7055");
    ASSERT_GE(index, 0);
    for (const auto operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        auto too_small = build_subaru_denso_sh7055_02_plan(
            operation, "sub_ecu_denso_sh7055_02", "SH7055",
            bytes::Bytes(flashdevices[index].romsize - 1, bytes::Byte{0}), test_kernel());
        ASSERT_FALSE(too_small.has_value());
        EXPECT_EQ(too_small.error().kind, ErrorKind::InvalidConfig);

        auto exact = build_subaru_denso_sh7055_02_plan(
            operation, "sub_ecu_denso_sh7055_02", "SH7055",
            bytes::Bytes(flashdevices[index].romsize, bytes::Byte{0}), test_kernel());
        ASSERT_TRUE(exact.has_value()) << exact.error().detail;
        ASSERT_TRUE(exact->image().has_value());
        EXPECT_EQ(exact->image()->size(), flashdevices[index].romsize);
    }
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRejectsWrongTesterId)
{
    auto fields = valid_sh7055_02_fields();
    std::get<SubaruDensoSh7055_02Plan>(fields.family_plan).tester_id = 0xf1;
    auto plan = validate_and_build(std::move(fields));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    auto valid = validate_subaru_denso_sh7055_02_plan(*plan);

    ASSERT_FALSE(valid.has_value());
    EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRejectsWrongTargetId)
{
    auto fields = valid_sh7055_02_fields();
    std::get<SubaruDensoSh7055_02Plan>(fields.family_plan).target_id = 0x11;
    auto plan = validate_and_build(std::move(fields));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    auto valid = validate_subaru_denso_sh7055_02_plan(*plan);

    ASSERT_FALSE(valid.has_value());
    EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRequiresOperationSpecificEcuIdRead)
{
    for (const auto operation : {FlashOperation::Read, FlashOperation::TestWrite})
    {
        auto fields = valid_sh7055_02_fields(operation);
        auto& family = std::get<SubaruDensoSh7055_02Plan>(fields.family_plan);
        family.read_ecu_id = !family.read_ecu_id;
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;

        auto valid = validate_subaru_denso_sh7055_02_plan(*plan);

        ASSERT_FALSE(valid.has_value());
        EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRejectsEraseRegions)
{
    auto fields = valid_sh7055_02_fields(FlashOperation::TestWrite);
    fields.erase_regions.push_back({.start = 0, .length = 0x1000});
    auto plan = validate_and_build(std::move(fields));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    auto valid = validate_subaru_denso_sh7055_02_plan(*plan);

    ASSERT_FALSE(valid.has_value());
    EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRejectsWrongTransferRegion)
{
    const int index = find_flash_device_index("SH7055");
    ASSERT_GE(index, 0);
    for (const auto region : {MemoryRegion{flashdevices[index].fblocks[0].start + 1, flashdevices[index].romsize},
                              MemoryRegion{flashdevices[index].fblocks[0].start, flashdevices[index].romsize - 1}})
    {
        auto fields = valid_sh7055_02_fields();
        fields.transfer_region = region;
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;

        auto valid = validate_subaru_denso_sh7055_02_plan(*plan);

        ASSERT_FALSE(valid.has_value());
        EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoSh7055_02Plan, ValidatorRequiresOnlyCycleIgnitionConfirmation)
{
    for (const auto& confirmations : {
             std::vector<ConfirmationSpec>{},
             std::vector<ConfirmationSpec>{
                 ConfirmationSpec{.id = ConfirmationSpec::Id::BeginEepromRead}},
             std::vector<ConfirmationSpec>{ConfirmationSpec{
                 .id = ConfirmationSpec::Id::CycleIgnition,
                 .arguments = {{"unexpected", "argument"}},
             }},
         })
    {
        auto fields = valid_sh7055_02_fields();
        fields.confirmations = confirmations;
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;

        auto valid = validate_subaru_denso_sh7055_02_plan(*plan);

        ASSERT_FALSE(valid.has_value());
        EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoSh7055_02Plan, KernelUploadAcceptsExactLowerAndUpperBoundaries)
{
    constexpr std::uint32_t kKernelStart = 0xFFFF6004;
    constexpr std::uint32_t kKernelEnd = kKernelStart + 0x6000;
    for (KernelImage kernel : {
             KernelImage{.id = "lower", .load_address = kKernelStart, .bytes = {0x01}},
             KernelImage{.id = "upper",
                         .load_address = kKernelEnd - 4,
                         .bytes = {0x01, 0x02, 0x03, 0x04}},
         })
    {
        auto plan = build_subaru_denso_sh7055_02_plan(
            FlashOperation::Read, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
            std::move(kernel));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    }
}

TEST(SubaruDensoSh7055_02Plan, KernelUploadRejectsAddressAndPaddedFootprintOutsideModelRegion)
{
    constexpr std::uint32_t kKernelStart = 0xFFFF6004;
    constexpr std::uint32_t kKernelEnd = kKernelStart + 0x6000;
    for (KernelImage kernel : {
             KernelImage{.id = "below", .load_address = kKernelStart - 1, .bytes = {0x01}},
             KernelImage{.id = "padded-past-end",
                         .load_address = kKernelEnd - 4,
                         .bytes = {0x01, 0x02, 0x03, 0x04, 0x05}},
         })
    {
        auto plan = build_subaru_denso_sh7055_02_plan(
            FlashOperation::Read, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
            std::move(kernel));
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoSh7055_02Plan, KernelUploadRejectsLengthOutsideThreeByteWireField)
{
    bytes::Bytes too_large(0x00FFFFF9, bytes::Byte{0});
    auto plan = build_subaru_denso_sh7055_02_plan(
        FlashOperation::Read, "sub_ecu_denso_sh7055_02", "SH7055", std::nullopt,
        KernelImage{.id = "wire-overflow", .load_address = 0xFFFF6004, .bytes = std::move(too_large)});

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(plan.error().detail.find("24-bit"), std::string::npos);
}

} // namespace
} // namespace fastecu::flash
