#include "src/algorithms/crypto/subaru_key_recovery.h"

#include <array>
#include <cstdint>
#include <format>

#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace
{

using subaru_key_recovery::Failure;
using subaru_key_recovery::Keys;

// The round keys the dialog's original author left commented out as a test
// vector, in round order.
constexpr Keys kKeys{0x3b61, 0x8bef, 0x9e51, 0x1075};

std::uint32_t xorshift(std::uint32_t& state)
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

struct Pair
{
    bytes::Bytes plain;
    bytes::Bytes cipher;
};

// 0x20000 bytes of xorshift32 plaintext (seed 0x2545f491), with every 16th
// word repeating word i/2 so duplicate plaintexts are present, and its
// encryption under kKeys.
Pair syntheticPair()
{
    std::array<std::uint32_t, 0x8000> words{};
    std::uint32_t state = 0x2545f491;
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        words[i] = i % 16 == 15 ? words[i / 2] : xorshift(state);
    }
    Pair pair;
    for (const std::uint32_t word : words)
    {
        bytes::appendU32Be(pair.plain, word);
        bytes::appendU32Be(pair.cipher, subaru_key_recovery::encrypt(word, kKeys));
    }
    return pair;
}

void overwriteWord(bytes::Bytes& data, std::size_t index, std::uint32_t word)
{
    bytes::Bytes encoded;
    bytes::appendU32Be(encoded, word);
    std::ranges::copy(encoded, data.begin() + static_cast<std::ptrdiff_t>(index * 4));
}

// Golden values below were computed by a Qt-free copy of the
// GetKeyOperationsSubaru helpers and attack this unit replaced.

TEST(SubaruKeyRecovery, FFunctionMatchesTheDialogImplementation)
{
    struct Case
    {
        std::uint16_t word;
        std::uint16_t key;
        std::uint16_t expected;
    };
    static constexpr auto kCases = std::to_array<Case>({{0x0000, 0x0000, 0xaaaa},
                                                        {0xffff, 0x0000, 0x1111},
                                                        {0x1234, 0x3b61, 0x4da4},
                                                        {0xbeef, 0x8bef, 0xa256},
                                                        {0x8000, 0x0001, 0xcaaa},
                                                        {0x5a5a, 0xa5a5, 0x1111}});
    for (const Case& test : kCases)
    {
        SCOPED_TRACE(std::format("word {:#06x} key {:#06x}", test.word, test.key));
        EXPECT_EQ(subaru_key_recovery::f_function(test.word, test.key), test.expected);
    }
}

TEST(SubaruKeyRecovery, EncryptMatchesTheDialogImplementation)
{
    struct Case
    {
        std::uint32_t plain;
        std::uint32_t expected;
    };
    static constexpr auto kCases = std::to_array<Case>({{0x00000000, 0x777dc7f1},
                                                        {0xffffffff, 0xeb20163a},
                                                        {0x12345678, 0x76521e57},
                                                        {0xdeadbeef, 0x47e819ed},
                                                        {0x80000001, 0xf96d8aed}});
    for (const Case& test : kCases)
    {
        SCOPED_TRACE(std::format("plain {:#010x}", test.plain));
        EXPECT_EQ(subaru_key_recovery::encrypt(test.plain, kKeys), test.expected);
    }
}

TEST(SubaruKeyRecovery, EncryptIsTheSsmPayloadCipher)
{
    for (const std::uint32_t plain : {0x00000000U, 0xffffffffU, 0x12345678U, 0xdeadbeefU, 0x80000001U})
    {
        SCOPED_TRACE(std::format("plain {:#010x}", plain));
        bytes::Bytes encoded;
        bytes::appendU32Be(encoded, plain);
        const bytes::Bytes payload =
            ssm_protocol::calculatePayload(bytes::ByteView(encoded), 4, kKeys, ssm_protocol::kIndexTransformationStock);
        ASSERT_EQ(payload.size(), 4U);
        EXPECT_EQ(subaru_key_recovery::encrypt(plain, kKeys), bytes::readU32Be(bytes::ByteView(payload)));
    }
}

// 0x8000 words, of which every 16th repeats an earlier one.
constexpr std::size_t kDistinctPairs = 0x8000 - 0x8000 / 16;

TEST(SubaruKeyRecovery, RecoversTheRealKeysFromASyntheticPair)
{
    const Pair pair = syntheticPair();
    const auto recovery = subaru_key_recovery::recover_keys(bytes::ByteView(pair.plain), bytes::ByteView(pair.cipher));
    ASSERT_TRUE(recovery.has_value());
    EXPECT_EQ(recovery->keys, kKeys);
    EXPECT_EQ(recovery->distinct_pairs, kDistinctPairs);
    EXPECT_EQ(recovery->reproduced_pairs, kDistinctPairs);
}

TEST(SubaruKeyRecovery, ReadsOnlyTheFirst128KiB)
{
    Pair pair = syntheticPair();
    pair.plain.resize(pair.plain.size() + 8, 0x5a);
    pair.cipher.resize(pair.cipher.size() + 4, 0xa5);
    const auto recovery = subaru_key_recovery::recover_keys(bytes::ByteView(pair.plain), bytes::ByteView(pair.cipher));
    ASSERT_TRUE(recovery.has_value());
    EXPECT_EQ(recovery->keys, kKeys);
    EXPECT_EQ(recovery->distinct_pairs, kDistinctPairs);
}

TEST(SubaruKeyRecovery, ShortInputFailsBeforeReading)
{
    const Pair pair = syntheticPair();
    const bytes::ByteView full_plain(pair.plain);
    const bytes::ByteView full_cipher(pair.cipher);
    EXPECT_EQ(subaru_key_recovery::recover_keys(full_plain.first(full_plain.size() - 1), full_cipher),
              std::unexpected(Failure::kInputTooShort));
    EXPECT_EQ(subaru_key_recovery::recover_keys(full_plain, full_cipher.first(full_cipher.size() - 1)),
              std::unexpected(Failure::kInputTooShort));
    EXPECT_EQ(subaru_key_recovery::recover_keys({}, {}), std::unexpected(Failure::kInputTooShort));
}

// Word 0x7fff repeats word 0x3fff, so 0x7ffe is the last distinct pair. Its
// ciphertext is chosen so that no k2 is consistent with it under the real k1
// and k4; every other pair outvotes it.
TEST(SubaruKeyRecovery, AMismatchedPairIsOutvoted)
{
    Pair pair = syntheticPair();
    overwriteWord(pair.cipher, 0x7ffe, 0xc0eb535f);
    const auto recovery = subaru_key_recovery::recover_keys(bytes::ByteView(pair.plain), bytes::ByteView(pair.cipher));
    ASSERT_TRUE(recovery.has_value());
    EXPECT_EQ(recovery->keys, kKeys);
    EXPECT_EQ(recovery->distinct_pairs, kDistinctPairs);
    EXPECT_EQ(recovery->reproduced_pairs, kDistinctPairs - 1);
}

// A ciphertext unrelated to the plaintext leaves k1 and k4 meaningless, so no
// k2 is consistent with a majority of the pairs.
TEST(SubaruKeyRecovery, UnrelatedFilesFailWithoutAMajorityKey)
{
    Pair pair = syntheticPair();
    pair.cipher.clear();
    std::uint32_t state = 0x9e3779b9;
    for (std::size_t i = 0; i < 0x8000; ++i)
    {
        bytes::appendU32Be(pair.cipher, xorshift(state));
    }
    EXPECT_EQ(subaru_key_recovery::recover_keys(bytes::ByteView(pair.plain), bytes::ByteView(pair.cipher)),
              std::unexpected(Failure::kNoMatchingKey));
}

} // namespace
