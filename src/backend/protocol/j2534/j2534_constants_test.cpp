#include "src/backend/protocol/j2534/j2534_constants.h"
#include "src/backend/protocol/j2534/tactrix_constants.h"

#include <cstdint>

#include <gtest/gtest.h>

TEST(J2534Constants, CanFlagsComposeAndClearIndependently)
{
    constexpr std::uint32_t kExtendedId = kJ2534Can29BitId;
    constexpr std::uint32_t kFramePadding = kJ2534Iso15765FramePad;
    constexpr std::uint32_t kExtendedAddress = kJ2534Iso15765ExtAddr;
    const std::uint32_t flags = kExtendedId | kFramePadding | kExtendedAddress;
    EXPECT_EQ(flags, 0x000001C0U);
    EXPECT_EQ(flags & ~kExtendedId, 0x000000C0U);
}

TEST(J2534Constants, VoltageSentinelsDoNotSignExtendOnWiderHosts)
{
    EXPECT_EQ(static_cast<std::uint64_t>(kJ2534ShortToGround), UINT64_C(0xFFFFFFFE));
    EXPECT_EQ(static_cast<std::uint64_t>(kJ2534VoltageOff), UINT64_C(0xFFFFFFFF));
}

TEST(J2534Constants, TactrixLineAliasesSelectTheExpectedChannels)
{
    EXPECT_EQ(kJ2534Iso9141K, 0x9240);
    EXPECT_EQ(kJ2534Iso9141L, 0x9241);
    EXPECT_EQ(kJ2534Iso9141Inno, 0x9242);
    EXPECT_EQ(kJ2534Iso14230K, 0x9320);
    EXPECT_EQ(kJ2534Iso14230L, 0x9321);
}
