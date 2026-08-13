#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

FlashPlanFields valid_mc_fields()
{
    const flashdev_t *device = find_flash_device("MC68HC16Y5");
    return {
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruDensoMc68hc16y5_02,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_denso_mc68hc16y5_02",
        .mcu_name = "MC68HC16Y5",
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}},
        .family_plan = SubaruDensoMc68hc16y5_02Plan{
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
    auto plan = build_subaru_denso_mc68hc16y5_02_plan(
        FlashOperation::Read, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
        std::nullopt, KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoMc68hc16y5_02);
    const auto& family = std::get<SubaruDensoMc68hc16y5_02Plan>(plan->family_plan());
    EXPECT_EQ(family.kernel_baud, 9600);
    EXPECT_EQ(family.encryption_xor, 0x55);
    EXPECT_EQ(family.kernel_magic, 0x3941);
    EXPECT_EQ(family.bootloader_ok, (std::array<std::uint8_t, 3>{0x4D, 0x00, 0xB3}));
}

TEST(SubaruDensoMc68hc16y5_02Plan, BuildsEcutekPlanForSuffixedProtocol)
{
    auto plan = build_subaru_denso_mc68hc16y5_02_plan(
        FlashOperation::Read, "sub_ecu_denso_mc68hc16y5_02_ecutek", "MC68HC16Y5",
        std::nullopt, KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const auto& family = std::get<SubaruDensoMc68hc16y5_02Plan>(plan->family_plan());
    EXPECT_EQ(family.kernel_baud, 11700);
    EXPECT_EQ(family.encryption_xor, 0x51);
    EXPECT_EQ(family.kernel_magic, 0x3940);
}

TEST(SubaruDensoMc68hc16y5_02Plan, RejectsRevision04Entirely)
{
    for (auto *name : {"sub_ecu_denso_mc68hc16y5_04", "sub_ecu_denso_mc68hc16y5_04_ecutek"})
    {
        auto plan = build_subaru_denso_mc68hc16y5_02_plan(
            FlashOperation::Read, name, "MC68HC16Y5", std::nullopt,
            KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, RejectsUnknownMcu)
{
    auto plan = build_subaru_denso_mc68hc16y5_02_plan(
        FlashOperation::Read, "sub_ecu_denso_mc68hc16y5_02", "NOT_A_REAL_MCU",
        std::nullopt, KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
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
        auto plan = build_subaru_denso_mc68hc16y5_02_plan(
            FlashOperation::Read, protocol, mcu, std::nullopt,
            KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
        ASSERT_FALSE(plan.has_value()) << protocol << " / " << mcu;
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, WriteRequiresImageOfExactRomSize)
{
    auto missing = build_subaru_denso_mc68hc16y5_02_plan(
        FlashOperation::Write, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
        std::nullopt, KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind, ErrorKind::InvalidConfig);

    const int index = find_flash_device_index("MC68HC16Y5");
    ASSERT_GE(index, 0);
    bytes::Bytes rom(flashdevices[index].romsize, bytes::Byte{0});
    auto ok = build_subaru_denso_mc68hc16y5_02_plan(
        FlashOperation::Write, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5", rom,
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
    EXPECT_TRUE(ok.has_value()) << ok.error().detail;
}

TEST(SubaruDensoMc68hc16y5_02Plan, StockAndEcutekTestWritesCarryExactImage)
{
    const int index = find_flash_device_index("MC68HC16Y5");
    ASSERT_GE(index, 0);
    for (const auto *protocol : {"sub_ecu_denso_mc68hc16y5_02",
                                 "sub_ecu_denso_mc68hc16y5_02_ecutek"})
    {
        bytes::Bytes rom(flashdevices[index].romsize, bytes::Byte{0});
        auto plan = build_subaru_denso_mc68hc16y5_02_plan(
            FlashOperation::TestWrite, protocol, "MC68HC16Y5", std::move(rom),
            KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        ASSERT_TRUE(plan->image().has_value());
        EXPECT_EQ(plan->image()->size(), flashdevices[index].romsize);
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, TpuRejectsWriteAndTestWrite)
{
    const int index = find_flash_device_index("MC68HC16Y5_TPU");
    ASSERT_GE(index, 0);
    for (const auto operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        bytes::Bytes rom(flashdevices[index].romsize, bytes::Byte{0});
        auto plan = build_subaru_denso_mc68hc16y5_02_plan(
            operation, "sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5_TPU", std::move(rom),
            KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, ValidatorRejectsEveryNonCanonicalWireField)
{
    for (int field = 0; field < 5; ++field)
    {
        auto fields = valid_mc_fields();
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
        }
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto valid = validate_subaru_denso_mc68hc16y5_02_plan(*plan);
        EXPECT_FALSE(valid.has_value()) << "field " << field;
        EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoMc68hc16y5_02Plan, ValidatorRejectsTransferEraseAndConfirmationDrift)
{
    for (int field = 0; field < 3; ++field)
    {
        auto fields = valid_mc_fields();
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
            fields.confirmations.push_back(
                ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition});
        }
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        EXPECT_FALSE(validate_subaru_denso_mc68hc16y5_02_plan(*plan).has_value());
    }

    auto fields = valid_mc_fields();
    fields.operation = FlashOperation::TestWrite;
    fields.image = bytes::Bytes(0x28000, 0);
    fields.erase_regions.push_back({.start = 0, .length = 0x1000});
    auto plan = validate_and_build(std::move(fields));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_FALSE(validate_subaru_denso_mc68hc16y5_02_plan(*plan).has_value());
}

TEST(SubaruDensoMc68hc16y5_02Plan, KernelUploadRequiresCanonicalAddressAndPaddedModelFit)
{
    auto exact = build_subaru_denso_mc68hc16y5_02_plan(
        FlashOperation::Read, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
        std::nullopt,
        KernelImage{.id = "full-region", .load_address = 0x20000, .bytes = bytes::Bytes(0x8000, 0)});
    ASSERT_TRUE(exact.has_value()) << exact.error().detail;

    for (KernelImage kernel : {
             KernelImage{.id = "shifted", .load_address = 0x20010, .bytes = {0xaa}},
             KernelImage{.id = "padded-past-end", .load_address = 0x20000, .bytes = bytes::Bytes(0x8001, 0)},
         })
    {
        auto plan = build_subaru_denso_mc68hc16y5_02_plan(
            FlashOperation::Read, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
            std::nullopt, std::move(kernel));
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
}

} // namespace
} // namespace fastecu::flash
