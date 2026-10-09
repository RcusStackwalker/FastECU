#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

FlashPlanFields ValidMcFields()
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    return {
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kSubaruDensoMc68hc16y502,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_denso_mc68hc16y5_02",
        .mcu_name = "MC68HC16Y5",
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}},
        .family_plan =
            SubaruDensoMc68hc16y5_02Plan{
                .connect_baud = 9600,
                .kernel_baud = 9600,
                .encryption_xor = 0x55,
                .kernel_magic = 0x3941,
                .bootloader_ok = {0x4d, 0x00, 0xb3},
            },
    };
}

TEST(SubaruDensoMc68hc16y5_02Plan, BuildsStockPlanForBareProtocol)
{
    auto plan = BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kRead, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
                                                 std::nullopt,
                                                 KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->Family(), FlashFamily::kSubaruDensoMc68hc16y502);
    const auto& family = std::get<SubaruDensoMc68hc16y5_02Plan>(plan->FamilyPlan());
    EXPECT_EQ(family.kernel_baud, 9600);
    EXPECT_EQ(family.encryption_xor, 0x55);
    EXPECT_EQ(family.kernel_magic, 0x3941);
    EXPECT_EQ(family.bootloader_ok, (std::array<std::uint8_t, 3>{0x4D, 0x00, 0xB3}));
}

TEST(SubaruDensoMc68hc16y5_02Plan, BuildsEcutekPlanForSuffixedProtocol)
{
    auto plan = BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kRead, "sub_ecu_denso_mc68hc16y5_02_ecutek",
                                                 "MC68HC16Y5", std::nullopt,
                                                 KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto& family = std::get<SubaruDensoMc68hc16y5_02Plan>(plan->FamilyPlan());
    EXPECT_EQ(family.kernel_baud, 11700);
    EXPECT_EQ(family.encryption_xor, 0x51);
    EXPECT_EQ(family.kernel_magic, 0x3940);
}

TEST(SubaruDensoMc68hc16y5_02Plan, Revision04IsNotAProtocolOfThisFamily)
{
    for (auto *name : {"sub_ecu_denso_mc68hc16y5_04", "sub_ecu_denso_mc68hc16y5_04_ecutek"})
    {
        ASSERT_THAT(BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kRead, name, "MC68HC16Y5", std::nullopt,
                                                     KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}}),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, RejectsUnknownMcu)
{
    ASSERT_THAT(BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kRead, "sub_ecu_denso_mc68hc16y5_02", "NOT_A_REAL_MCU",
                                                 std::nullopt,
                                                 KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}}),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02Plan, RejectsEveryKnownButWrongProtocolMcuPair)
{
    for (const auto& [protocol, mcu] : {
             std::pair{"sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5_TPU"},
             std::pair{"sub_ecu_denso_mc68hc16y5_02_ecutek", "MC68HC16Y5_TPU"},
             std::pair{"sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5"},
             std::pair{"sub_ecu_denso_mc68hc16y5_02", "SH7055"},
         })
    {
        ASSERT_THAT(BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kRead, protocol, mcu, std::nullopt,
                                                     KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}}),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig))
            << protocol << " / " << mcu;
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, WriteRequiresImageOfExactRomSize)
{
    ASSERT_THAT(BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kWrite, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
                                                 std::nullopt,
                                                 KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}}),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));

    const int index = FindFlashDeviceIndex("MC68HC16Y5");
    ASSERT_GE(index, 0);
    bytes::Bytes rom(kFlashDevices[index].romsize, bytes::Byte{0});
    EXPECT_THAT(BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kWrite, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
                                                 rom, KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}}),
                fastecu::testing::IsOk());
}

TEST(SubaruDensoMc68hc16y5_02Plan, StockAndEcutekTestWritesCarryExactImage)
{
    const int index = FindFlashDeviceIndex("MC68HC16Y5");
    ASSERT_GE(index, 0);
    for (const auto *protocol : {"sub_ecu_denso_mc68hc16y5_02", "sub_ecu_denso_mc68hc16y5_02_ecutek"})
    {
        bytes::Bytes rom(kFlashDevices[index].romsize, bytes::Byte{0});
        auto plan = BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kTestWrite, protocol, "MC68HC16Y5", std::move(rom),
                                                     KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ASSERT_TRUE(plan->Image().has_value());
        EXPECT_EQ(plan->Image()->size(), kFlashDevices[index].romsize);
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, TpuRejectsWriteAndTestWrite)
{
    const int index = FindFlashDeviceIndex("MC68HC16Y5_TPU");
    ASSERT_GE(index, 0);
    for (const auto operation : {FlashOperation::kWrite, FlashOperation::kTestWrite})
    {
        bytes::Bytes rom(kFlashDevices[index].romsize, bytes::Byte{0});
        ASSERT_THAT(BuildSubaruDensoMc68hc16y502Plan(operation, "sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5_TPU",
                                                     std::move(rom),
                                                     KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}}),
                    fastecu::testing::IsErr(ErrorKind::kUnsupported));
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, ValidatorRejectsEveryNonCanonicalWireField)
{
    for (int field = 0; field < 5; ++field)
    {
        auto fields = ValidMcFields();
        auto& wire = std::get<SubaruDensoMc68hc16y5_02Plan>(fields.family_plan);
        switch (field)
        {
        case 0:
            wire.connect_baud = 9599;
            break;
        case 1:
            wire.kernel_baud = 9599;
            break;
        case 2:
            wire.encryption_xor = 0x54;
            break;
        case 3:
            wire.kernel_magic = 0x3940;
            break;
        case 4:
            wire.bootloader_ok[2] = 0xb4;
            break;
        default:
            FAIL() << "unexpected field " << field;
        }
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        auto valid = ValidateSubaruDensoMc68hc16y502Plan(*plan);
        EXPECT_THAT(valid, ::testing::Not(fastecu::testing::IsOk())) << "field " << field;
        EXPECT_THAT(valid, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, ValidatorRejectsTransferEraseAndConfirmationDrift)
{
    for (int field = 0; field < 3; ++field)
    {
        auto fields = ValidMcFields();
        if (field == 0)
        {
            ++fields.transfer_region.start;
        }
        else if (field == 1)
        {
            --fields.transfer_region.length;
        }
        else
        {
            fields.confirmations.push_back(ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition});
        }
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_THAT(ValidateSubaruDensoMc68hc16y502Plan(*plan), ::testing::Not(fastecu::testing::IsOk()));
    }

    auto fields = ValidMcFields();
    fields.operation = FlashOperation::kTestWrite;
    fields.image = bytes::Bytes(0x28000, 0);
    fields.erase_regions.push_back({.start = 0, .length = 0x1000});
    auto plan = ValidateAndBuild(std::move(fields));
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(ValidateSubaruDensoMc68hc16y502Plan(*plan), ::testing::Not(fastecu::testing::IsOk()));
}

TEST(SubaruDensoMc68hc16y5_02Plan, KernelUploadRequiresCanonicalAddressAndPaddedModelFit)
{
    ASSERT_THAT(BuildSubaruDensoMc68hc16y502Plan(
                    FlashOperation::kRead, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5", std::nullopt,
                    KernelImage{.id = "full-region", .load_address = 0x20000, .bytes = bytes::Bytes(0x8000, 0)}),
                fastecu::testing::IsOk());

    for (KernelImage kernel : {
             KernelImage{.id = "shifted", .load_address = 0x20010, .bytes = {0xaa}},
             KernelImage{.id = "padded-past-end", .load_address = 0x20000, .bytes = bytes::Bytes(0x8001, 0)},
         })
    {
        ASSERT_THAT(BuildSubaruDensoMc68hc16y502Plan(FlashOperation::kRead, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
                                                     std::nullopt, std::move(kernel)),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

} // namespace
} // namespace fastecu::flash
