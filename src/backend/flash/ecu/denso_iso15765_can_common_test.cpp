#include "src/backend/flash/ecu/denso_iso15765_can_common.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{
namespace
{

// Two layers of assertion here, and they catch different mistakes.
//
// The table tests below pin the byte values, transcribed a second time from
// the same legacy generate_can_seed_key/encrypt_payload/decrypt_payload lines
// the header cites, so a slipped digit in the header fails here.
//
// The vector tests pin what those tables actually PRODUCE when run through
// SsmProtocol. They exist because the four executor suites only ever compare
// the executor's crypto against their own re-derivation of it: both sides go
// through SsmProtocol, so a change to SsmProtocol's Feistel arithmetic moves
// both sides together and every one of those suites keeps passing while the
// bytes on the wire change. These fixed vectors do not move. They were
// computed by an independent reimplementation of transformWord() from
// src/algorithms/protocol/ssm/ssm_protocol_core.cpp and then confirmed
// against the compiled implementation; they are a regression pin, not an
// ECU-sourced golden -- no legacy source publishes a test vector.

TEST(DensoIso15765CanCommonTest, SeedKeyTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 16> kExpected{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                                      0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
    EXPECT_EQ(kDensoIso15765SeedKeyTable, kExpected);
}

TEST(DensoIso15765CanCommonTest, EncryptTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 4> kExpected{0xC85B, 0x32C0, 0xE282, 0x92A0};
    EXPECT_EQ(kDensoIso15765EncryptTable, kExpected);
}

TEST(DensoIso15765CanCommonTest, DecryptTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 4> kExpected{0x92A0, 0xE282, 0x32C0, 0xC85B};
    EXPECT_EQ(kDensoIso15765DecryptTable, kExpected);
}

TEST(DensoIso15765CanCommonTest, IndexTransformationMatchesLegacyValues)
{
    constexpr std::array<std::uint8_t, 32> kExpected{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                     0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                     0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    EXPECT_EQ(kDensoIso15765IndexTransformation, kExpected);
}

// All four legacy sources spell the decrypt table out rather than deriving it
// from the encrypt table, so the reversal relationship calculatePayload relies
// on to invert is pinned here rather than assumed.
TEST(DensoIso15765CanCommonTest, DecryptTableIsEncryptTableReversed)
{
    ASSERT_EQ(kDensoIso15765EncryptTable.size(), kDensoIso15765DecryptTable.size());
    for (std::size_t i = 0; i < kDensoIso15765EncryptTable.size(); ++i)
    {
        EXPECT_EQ(kDensoIso15765EncryptTable[i], kDensoIso15765DecryptTable[kDensoIso15765EncryptTable.size() - 1 - i]);
    }
}

TEST(DensoIso15765CanCommonTest, SeedKeyProducesKnownVectors)
{
    const bytes::Bytes kSeedA{0x11, 0x22, 0x33, 0x44};
    EXPECT_EQ(SsmProtocol::calculateSeedKey(kSeedA, kDensoIso15765SeedKeyTable, kDensoIso15765IndexTransformation),
              (bytes::Bytes{0x35, 0xB6, 0x83, 0xBF}));

    const bytes::Bytes kSeedB{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(SsmProtocol::calculateSeedKey(kSeedB, kDensoIso15765SeedKeyTable, kDensoIso15765IndexTransformation),
              (bytes::Bytes{0xB6, 0xF5, 0x24, 0x21}));
}

TEST(DensoIso15765CanCommonTest, EncryptProducesKnownPayloadVector)
{
    const bytes::Bytes kPlain{0x00, 0x01, 0x02, 0x03, 0xFC, 0xFD, 0xFE, 0xFF};
    EXPECT_EQ(SsmProtocol::calculatePayload(kPlain, static_cast<std::uint32_t>(kPlain.size()),
                                            kDensoIso15765EncryptTable, kDensoIso15765IndexTransformation),
              (bytes::Bytes{0xE0, 0xD3, 0x85, 0x2B, 0xC5, 0xFE, 0x4B, 0x39}));
}

// The dump path decrypts each 256-byte page with the decrypt table; the write
// path encrypts the whole image with the encrypt table. A page that survives
// encrypt-then-decrypt unchanged is what makes reading back a freshly written
// ROM meaningful, so pin the round trip and not only the one direction.
TEST(DensoIso15765CanCommonTest, DecryptInvertsEncrypt)
{
    const bytes::Bytes kCipher{0xE0, 0xD3, 0x85, 0x2B, 0xC5, 0xFE, 0x4B, 0x39};
    EXPECT_EQ(SsmProtocol::calculatePayload(kCipher, static_cast<std::uint32_t>(kCipher.size()),
                                            kDensoIso15765DecryptTable, kDensoIso15765IndexTransformation),
              (bytes::Bytes{0x00, 0x01, 0x02, 0x03, 0xFC, 0xFD, 0xFE, 0xFF}));
}

} // namespace
} // namespace fastecu::flash
