#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

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
    const int index = find_flash_device_index("MC68HC16Y5");
    ASSERT_GE(index, 0);
    for (const auto operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        bytes::Bytes rom(flashdevices[index].romsize, bytes::Byte{0});
        auto plan = build_subaru_denso_mc68hc16y5_02_plan(
            operation, "sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5", std::move(rom),
            KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}});
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
    }
}

} // namespace
} // namespace fastecu::flash
