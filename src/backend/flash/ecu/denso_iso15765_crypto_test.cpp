#include "src/backend/flash/ecu/denso_iso15765_crypto.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"

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
// SsmProtocol. They exist because the executor suites can compare the
// executor's crypto against an independent literal transcript while still
// exercising the same SsmProtocol implementation: a change to Feistel
// arithmetic could otherwise move derived fixtures and wire output together.
// These fixed vectors do not move. They were
// computed by an independent reimplementation of transformWord() from
// src/algorithms/protocol/ssm/ssm_protocol_core.cpp and then confirmed
// against the compiled implementation; they are a regression pin, not an
// ECU-sourced golden -- no legacy source publishes a test vector.

TEST(DensoIso15765Crypto, SeedKeyTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 16> kExpected{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                                      0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
    EXPECT_EQ(kDensoIso15765SeedKeyTable, kExpected);
}

TEST(DensoIso15765Crypto, EncryptTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 4> kExpected{0xC85B, 0x32C0, 0xE282, 0x92A0};
    EXPECT_EQ(kDensoIso15765EncryptTable, kExpected);
}

TEST(DensoIso15765Crypto, DecryptTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 4> kExpected{0x92A0, 0xE282, 0x32C0, 0xC85B};
    EXPECT_EQ(kDensoIso15765DecryptTable, kExpected);
}

// The applicable legacy sources spell the decrypt table out rather than
// deriving it from the encrypt table, so the reversal relationship
// calculatePayload relies on to invert is pinned here rather than assumed.
// The BEEF-dialect diesel read path is raw and therefore is not a decrypt consumer.
TEST(DensoIso15765Crypto, DecryptTableIsEncryptTableReversed)
{
    ASSERT_EQ(kDensoIso15765EncryptTable.size(), kDensoIso15765DecryptTable.size());
    for (std::size_t i = 0; i < kDensoIso15765EncryptTable.size(); ++i)
    {
        SCOPED_TRACE(::testing::PrintToString(i));
        EXPECT_EQ(kDensoIso15765EncryptTable[i], kDensoIso15765DecryptTable[kDensoIso15765EncryptTable.size() - 1 - i]);
    }
}

TEST(DensoIso15765Crypto, SeedKeyProducesKnownVectors)
{
    const bytes::Bytes seed_a{0x11, 0x22, 0x33, 0x44};
    EXPECT_THAT(ssm_protocol::CalculateSeedKey(seed_a, kDensoIso15765SeedKeyTable, ssm_protocol::kStockPairTable),
                test_bytes::BytesEq((bytes::Bytes{0x35, 0xB6, 0x83, 0xBF})));

    const bytes::Bytes seed_b{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_THAT(ssm_protocol::CalculateSeedKey(seed_b, kDensoIso15765SeedKeyTable, ssm_protocol::kStockPairTable),
                test_bytes::BytesEq((bytes::Bytes{0xB6, 0xF5, 0x24, 0x21})));
}

TEST(DensoIso15765Crypto, EncryptProducesKnownPayloadVector)
{
    const bytes::Bytes plain{0x00, 0x01, 0x02, 0x03, 0xFC, 0xFD, 0xFE, 0xFF};
    EXPECT_THAT(ssm_protocol::CalculatePayload(plain, kDensoIso15765EncryptTable, ssm_protocol::kStockPairTable),
                test_bytes::BytesEq((bytes::Bytes{0xE0, 0xD3, 0x85, 0x2B, 0xC5, 0xFE, 0x4B, 0x39})));
}

// The dump path decrypts each 256-byte page with the decrypt table; the write
// path encrypts the whole image with the encrypt table. A page that survives
// encrypt-then-decrypt unchanged is what makes reading back a freshly written
// ROM meaningful, so pin the round trip and not only the one direction.
TEST(DensoIso15765Crypto, DecryptInvertsEncrypt)
{
    const bytes::Bytes cipher{0xE0, 0xD3, 0x85, 0x2B, 0xC5, 0xFE, 0x4B, 0x39};
    EXPECT_THAT(ssm_protocol::CalculatePayload(cipher, kDensoIso15765DecryptTable, ssm_protocol::kStockPairTable),
                test_bytes::BytesEq((bytes::Bytes{0x00, 0x01, 0x02, 0x03, 0xFC, 0xFD, 0xFE, 0xFF})));
}

} // namespace
} // namespace fastecu::flash
