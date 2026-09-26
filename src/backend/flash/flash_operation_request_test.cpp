#include "src/backend/flash/flash_operation_request.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

TEST(FlashOperationFromCommand, MapsTheTwoWriteCommands)
{
    EXPECT_EQ(flash_operation_from_command("write"), FlashOperation::Write);
    EXPECT_EQ(flash_operation_from_command("test_write"), FlashOperation::TestWrite);
}

TEST(FlashOperationFromCommand, TreatsEveryOtherCommandAsRead)
{
    EXPECT_EQ(flash_operation_from_command("read"), FlashOperation::Read);
    EXPECT_EQ(flash_operation_from_command(""), FlashOperation::Read);
    EXPECT_EQ(flash_operation_from_command("Write"), FlashOperation::Read);
    EXPECT_EQ(flash_operation_from_command("test-write"), FlashOperation::Read);
}

TEST(IsDensoTcuProtocol, MatchesBothDensoTcuCanProtocols)
{
    EXPECT_TRUE(is_denso_tcu_protocol("sub_tcu_denso_sh7055_can"));
    EXPECT_TRUE(is_denso_tcu_protocol("sub_tcu_denso_sh7058_can"));
}

TEST(IsDensoTcuProtocol, RejectsNearMisses)
{
    EXPECT_FALSE(is_denso_tcu_protocol("sub_tcu_denso_sh7058_can_future"));
    EXPECT_FALSE(is_denso_tcu_protocol("sub_ecu_denso_sh7058_can"));
    EXPECT_FALSE(is_denso_tcu_protocol("sub_tcu_denso_sh7055"));
    EXPECT_FALSE(is_denso_tcu_protocol(""));
}

TEST(KernelPath, InsertsASeparatorOnlyWhenMissing)
{
    EXPECT_EQ(kernel_path("/k", "a.bin"), "/k/a.bin");
    EXPECT_EQ(kernel_path("/k/", "a.bin"), "/k/a.bin");
}

TEST(KernelPath, HandlesEmptyParts)
{
    EXPECT_EQ(kernel_path("", "a.bin"), "a.bin");
    EXPECT_EQ(kernel_path("/k", ""), "/k/");
}

TEST(ReadImageFilename, PrefixesTheRomId)
{
    EXPECT_EQ(read_image_filename("A2WC522N", "2026-09-26_08h00m00s"), "A2WC522N2026-09-26_08h00m00s.bin");
}

TEST(ReadImageFilename, FallsBackToReadImageWithoutARomId)
{
    EXPECT_EQ(read_image_filename("", "2026-09-26_08h00m00s"), "read_image_2026-09-26_08h00m00s.bin");
}

} // namespace
} // namespace fastecu::flash
