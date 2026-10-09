#include "src/backend/flash/flash_operation_request.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

TEST(FlashOperationFromCommand, MapsTheTwoWriteCommands)
{
    EXPECT_EQ(FlashOperationFromCommand("write"), FlashOperation::kWrite);
    EXPECT_EQ(FlashOperationFromCommand("test_write"), FlashOperation::kTestWrite);
}

TEST(FlashOperationFromCommand, TreatsEveryOtherCommandAsRead)
{
    EXPECT_EQ(FlashOperationFromCommand("read"), FlashOperation::kRead);
    EXPECT_EQ(FlashOperationFromCommand(""), FlashOperation::kRead);
    EXPECT_EQ(FlashOperationFromCommand("Write"), FlashOperation::kRead);
    EXPECT_EQ(FlashOperationFromCommand("test-write"), FlashOperation::kRead);
}

TEST(IsDensoTcuProtocol, MatchesBothDensoTcuCanProtocols)
{
    EXPECT_TRUE(IsDensoTcuProtocol("sub_tcu_denso_sh7055_can"));
    EXPECT_TRUE(IsDensoTcuProtocol("sub_tcu_denso_sh7058_can"));
}

TEST(IsDensoTcuProtocol, RejectsNearMisses)
{
    EXPECT_FALSE(IsDensoTcuProtocol("sub_tcu_denso_sh7058_can_future"));
    EXPECT_FALSE(IsDensoTcuProtocol("sub_ecu_denso_sh7058_can"));
    EXPECT_FALSE(IsDensoTcuProtocol("sub_tcu_denso_sh7055"));
    EXPECT_FALSE(IsDensoTcuProtocol(""));
}

TEST(KernelPath, InsertsASeparatorOnlyWhenMissing)
{
    EXPECT_EQ(KernelPath("/k", "a.bin"), "/k/a.bin");
    EXPECT_EQ(KernelPath("/k/", "a.bin"), "/k/a.bin");
}

TEST(KernelPath, HandlesEmptyParts)
{
    EXPECT_EQ(KernelPath("", "a.bin"), "a.bin");
    EXPECT_EQ(KernelPath("/k", ""), "/k/");
}

TEST(ReadImageFilename, PrefixesTheRomId)
{
    EXPECT_EQ(ReadImageFilename("A2WC522N", "2026-09-26_08h00m00s"), "A2WC522N2026-09-26_08h00m00s.bin");
}

TEST(ReadImageFilename, FallsBackToReadImageWithoutARomId)
{
    EXPECT_EQ(ReadImageFilename("", "2026-09-26_08h00m00s"), "read_image_2026-09-26_08h00m00s.bin");
}

} // namespace
} // namespace fastecu::flash
