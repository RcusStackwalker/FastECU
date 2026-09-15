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
}
} // namespace
} // namespace fastecu::flash
