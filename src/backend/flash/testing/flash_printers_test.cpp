#include "src/backend/flash/testing/flash_printers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{
TEST(FlashPrinters, PrintsAnOperationByName)
{
    EXPECT_EQ(::testing::PrintToString(FlashOperation::kRead), "Read");
    EXPECT_EQ(::testing::PrintToString(FlashOperation::kWrite), "Write");
    EXPECT_EQ(::testing::PrintToString(FlashOperation::kTestWrite), "TestWrite");
}

TEST(FlashPrinters, PrintsAMemoryRegionAsAHexStartAndLength)
{
    const MemoryRegion region{.start = 0x8000, .length = 0x137F00};

    EXPECT_EQ(::testing::PrintToString(region), "[0x8000, len 0x137f00)");
}

TEST(FlashPrinters, PrintsAFamilyByName)
{
    EXPECT_EQ(::testing::PrintToString(FlashFamily::kSubaruHitachiSh72543rCan), "SubaruHitachiSh72543rCan");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::kSubaruDensoSh72531Can), "SubaruDensoSh72531Can");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::kSubaruDensoSh705xDensoCan), "SubaruDensoSh705xDensoCan");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::kSubaruTcuDensoSh705xCan), "SubaruTcuDensoSh705xCan");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::kSubaruDensoSh7058Can), "SubaruDensoSh7058Can");
    EXPECT_EQ(::testing::PrintToString(FlashFamily::kSubaruDensoSh7058CanDiesel), "SubaruDensoSh7058CanDiesel");
}

TEST(FlashPrinters, PrintsMixedCanTransportByName)
{
    EXPECT_EQ(::testing::PrintToString(TransportKind::kCanRawIso15765), "CanRawIso15765");
}
} // namespace
} // namespace fastecu::flash
