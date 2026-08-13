#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

KernelImage test_kernel()
{
    return {.id = "k", .load_address = 0xffff6004, .bytes = {0xaa}};
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

} // namespace
} // namespace fastecu::flash
