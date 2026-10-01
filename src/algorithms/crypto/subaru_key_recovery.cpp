#include "src/algorithms/crypto/subaru_key_recovery.h"

#include <bit>
#include <span>
#include <unordered_set>
#include <vector>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace subaru_key_recovery
{
namespace
{

constexpr std::size_t kNybbles = 4;
// Each f-function S-box lookup takes five key bits, so each nybble has 0x20
// candidate subkeys.
constexpr std::uint32_t kSubkeys = 0x20;

// Bit positions numbered 1 (most significant) to 32 (least), two per nybble.
using BitPair = std::array<unsigned, 2>;
constexpr std::array<BitPair, kNybbles> kInBits{{{20, 21}, {24, 25}, {28, 29}, {32, 17}}};
constexpr std::array<BitPair, kNybbles> kOutBits{{{11, 8}, {15, 12}, {3, 16}, {7, 4}}};

struct WordPair
{
    std::uint32_t plain;
    std::uint32_t cipher;
};

std::uint32_t bit(std::uint32_t value, unsigned position)
{
    return (value >> (32U - position)) & 1U;
}

std::uint32_t bits(std::uint32_t value, const BitPair& positions, unsigned offset = 0)
{
    return bit(value, positions[0] + offset) ^ bit(value, positions[1] + offset);
}

std::uint16_t high(std::uint32_t word)
{
    return static_cast<std::uint16_t>(word >> 16U);
}

std::uint16_t low(std::uint32_t word)
{
    return static_cast<std::uint16_t>(word);
}

// The round key a nybble's subkey stands for. The fifth subkey bit of nybble
// 0 shifts out of the 16-bit key and is folded into bit 0 instead.
std::uint16_t candidate(std::size_t nybble, std::uint32_t subkey)
{
    auto key = static_cast<std::uint16_t>(subkey << (12U - nybble * 4U));
    if (nybble == 0 && subkey > 0xF)
    {
        key = static_cast<std::uint16_t>(key + 1U);
    }
    return key;
}

// Every pair whose plaintext word has not occurred earlier, in input order.
std::vector<WordPair> distinct_pairs(bytes::ByteView plain, bytes::ByteView cipher)
{
    std::vector<WordPair> pairs;
    std::unordered_set<std::uint32_t> seen;
    for (std::size_t offset = 0; offset < kAnalyzedBytes; offset += 4)
    {
        const std::uint32_t word = bytes::readU32Be(plain, offset);
        if (seen.insert(word).second)
        {
            pairs.push_back({word, bytes::readU32Be(cipher, offset)});
        }
    }
    return pairs;
}

// For each nybble, counts the pairs whose parity is zero under each candidate
// subkey and keeps the subkey whose count strays furthest from half the
// pairs; a tie goes to the higher subkey. Only the low four bits of each
// chosen subkey reach the key.
template <typename Parity> std::uint16_t most_biased_key(std::span<const WordPair> pairs, Parity parity)
{
    const std::size_t half = pairs.size() / 2;
    std::uint16_t key = 0;
    for (std::size_t nybble = 0; nybble < kNybbles; ++nybble)
    {
        std::array<std::size_t, kSubkeys> agreeing{};
        for (const WordPair& pair : pairs)
        {
            for (std::uint32_t subkey = 0; subkey < kSubkeys; ++subkey)
            {
                if (parity(pair, nybble, candidate(nybble, subkey)) == 0)
                {
                    ++agreeing[subkey];
                }
            }
        }
        std::uint32_t best = 0;
        std::size_t best_bias = 0;
        for (std::uint32_t subkey = 0; subkey < kSubkeys; ++subkey)
        {
            const std::size_t count = agreeing[subkey];
            const std::size_t bias = count > half ? count - half : half - count;
            if (bias >= best_bias)
            {
                best = subkey;
                best_bias = bias;
            }
        }
        key = static_cast<std::uint16_t>(key | ((best & 0xFU) << (12U - nybble * 4U)));
    }
    return key;
}

// The lowest key under which f(input, key) ^ mask == target.
std::expected<std::uint16_t, Failure> lowest_key(std::uint16_t input, std::uint16_t mask, std::uint16_t target)
{
    for (std::uint32_t key = 0; key <= 0xFFFFU; ++key)
    {
        if ((f_function(input, static_cast<std::uint16_t>(key)) ^ mask) == target)
        {
            return static_cast<std::uint16_t>(key);
        }
    }
    return std::unexpected(Failure::NoMatchingKey);
}

} // namespace

std::uint16_t f_function(std::uint16_t word, std::uint16_t key)
{
    std::uint32_t index = static_cast<std::uint32_t>(word ^ key);
    index += index << 16U;
    std::uint16_t substituted = 0;
    for (unsigned n = 0; n < kNybbles; ++n)
    {
        const std::uint32_t nibble = SsmProtocol::kIndexTransformationStock[(index >> (n * 4U)) & 0x1FU];
        substituted = static_cast<std::uint16_t>(substituted + (nibble << (n * 4U)));
    }
    return std::rotr(substituted, 3);
}

std::uint32_t encrypt(std::uint32_t plain, const Keys& keys)
{
    std::uint32_t word = plain;
    for (const std::uint16_t key : keys)
    {
        const auto mixed = static_cast<std::uint16_t>(high(word) ^ f_function(low(word), key));
        word = (static_cast<std::uint32_t>(low(word)) << 16U) | mixed;
    }
    return std::rotl(word, 16);
}

std::expected<Keys, Failure> recover_keys(bytes::ByteView plain, bytes::ByteView cipher)
{
    if (plain.size() < kAnalyzedBytes || cipher.size() < kAnalyzedBytes)
    {
        return std::unexpected(Failure::InputTooShort);
    }
    const std::vector<WordPair> pairs = distinct_pairs(plain, cipher);

    const std::uint16_t k4 = most_biased_key(
        pairs,
        [](const WordPair& pair, std::size_t nybble, std::uint16_t key)
        {
            const std::uint32_t p = bits(pair.plain, kInBits[nybble]) ^ bits(pair.plain, kOutBits[nybble]);
            const std::uint32_t c = bits(pair.cipher, kOutBits[nybble], 16);
            const auto x3 = static_cast<std::uint16_t>(high(pair.cipher) ^ f_function(low(pair.cipher), key));
            return p ^ c ^ bits(x3, kInBits[nybble]);
        });

    const std::uint16_t k1 = most_biased_key(
        pairs,
        [k4](const WordPair& pair, std::size_t nybble, std::uint16_t key)
        {
            const auto x3 = static_cast<std::uint16_t>(high(pair.cipher) ^ f_function(low(pair.cipher), k4));
            const std::uint32_t p = bits(pair.plain, kOutBits[nybble], 16);
            const std::uint32_t c = bits(x3, kOutBits[nybble], 16);
            const auto x2 = static_cast<std::uint16_t>(high(pair.plain) ^ f_function(low(pair.plain), key));
            return p ^ c ^ bits(x2, kInBits[nybble]);
        });

    // k2 and k3 from the last distinct pair: the two middle-round halves.
    const WordPair& last = pairs.back();
    const auto x2 = static_cast<std::uint16_t>(high(last.plain) ^ f_function(low(last.plain), k1));
    const auto x3 = static_cast<std::uint16_t>(high(last.cipher) ^ f_function(low(last.cipher), k4));
    const auto k2 = lowest_key(x2, low(last.plain), x3);
    if (!k2.has_value())
    {
        return std::unexpected(k2.error());
    }
    const auto k3 = lowest_key(x3, low(last.cipher), x2);
    if (!k3.has_value())
    {
        return std::unexpected(k3.error());
    }
    return Keys{k1, *k2, *k3, k4};
}

} // namespace subaru_key_recovery
