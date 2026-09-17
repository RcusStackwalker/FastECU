#include "src/backend/flash/testing/flash_printers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{
TEST(FlashPrinters, PrintsAnOperationByName)
{
    EXPECT_EQ(::testing::PrintToString(FlashOperation::Read), "Read");
    EXPECT_EQ(::testing::PrintToString(FlashOperation::Write), "Write");
    EXPECT_EQ(::testing::PrintToString(FlashOperation::TestWrite), "TestWrite");
}

TEST(FlashPrinters, PrintsAMemoryRegionAsAHexStartAndLength)
{
    const MemoryRegion region{.start = 0x8000, .length = 0x137F00};

    EXPECT_EQ(::testing::PrintToString(region), "[0x8000, len 0x137f00)");
}

TEST(FlashPrinters, PrintsAFamilyByName)
{
    EXPECT_EQ(::testing::PrintToString(FlashFamily::SubaruDensoSh72531Can), "SubaruDensoSh72531Can");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::SubaruDensoSh705xDensoCan), "SubaruDensoSh705xDensoCan");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::SubaruTcuDensoSh705xCan), "SubaruTcuDensoSh705xCan");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::SubaruDensoSh7058Can), "SubaruDensoSh7058Can");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::SubaruDensoSh7058CanDiesel), "SubaruDensoSh7058CanDiesel");
}

TEST(FlashPrinters, PrintsMixedCanTransportByName)
{
    EXPECT_EQ(::testing::PrintToString(TransportKind::CanRawIso15765), "CanRawIso15765");
}
} // namespace
} // namespace fastecu::flash
