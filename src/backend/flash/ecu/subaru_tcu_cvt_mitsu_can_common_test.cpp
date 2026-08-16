#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

// Pins the exact byte values transcribed from both legacy sources (see the
// header's citations). MH8111CanExecutorTest and MH8104CanExecutorTest
// already exercise these tables end-to-end via seed_key()/encrypt_rom()/
// decrypt_page() in their own connect/write flows; this test exists so a
// future edit to the shared table is caught here directly, not only
// indirectly through wire-byte assertions two files away.

TEST(SubaruTcuCvtMitsuCanCommonTest, SeedKeyTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 16> kExpected{0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
                                                      0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
    EXPECT_EQ(tcuCvtMitsuSeedKeyTable(), kExpected);
}

TEST(SubaruTcuCvtMitsuCanCommonTest, EncryptTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 4> kExpected{0x7bf2, 0xa8b4, 0x4492, 0x6587};
    EXPECT_EQ(tcuCvtMitsuEncryptTable(), kExpected);
}

TEST(SubaruTcuCvtMitsuCanCommonTest, DecryptTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 4> kExpected{0x6587, 0x4492, 0xa8b4, 0x7bf2};
    EXPECT_EQ(tcuCvtMitsuDecryptTable(), kExpected);
}

// The decrypt table is the encrypt table read back to front -- both legacy
// sources encode it that way rather than deriving it, so pin the
// relationship too, not just each table's own values.
TEST(SubaruTcuCvtMitsuCanCommonTest, DecryptTableIsEncryptTableReversed)
{
    const auto& encrypt = tcuCvtMitsuEncryptTable();
    const auto& decrypt = tcuCvtMitsuDecryptTable();
    ASSERT_EQ(encrypt.size(), decrypt.size());
    for (std::size_t i = 0; i < encrypt.size(); ++i)
    {
        EXPECT_EQ(encrypt[i], decrypt[encrypt.size() - 1 - i]);
    }
}

} // namespace
} // namespace fastecu::flash
