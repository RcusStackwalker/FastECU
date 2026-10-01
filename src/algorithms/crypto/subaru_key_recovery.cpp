#include "src/algorithms/crypto/subaru_key_recovery.h"

#include <algorithm>
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
constexpr std::uint32_t kKeySpace = 0x10000;

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

// f_function(word, key) depends only on word ^ key, so the keys that map
// word to a given output are word ^ v for each v whose f_function(v, 0) is
// that output. This holds every such v, grouped by output.
class FunctionInverse
{
  public:
    FunctionInverse()
    {
        std::vector<std::uint32_t> counts(kKeySpace);
        for (std::uint32_t v = 0; v < kKeySpace; ++v)
        {
            ++counts[f_function(static_cast<std::uint16_t>(v), 0)];
        }
        for (std::uint32_t output = 0; output < kKeySpace; ++output)
        {
            first_[output + 1] = first_[output] + counts[output];
        }
        std::vector<std::uint32_t> next(first_.begin(), first_.end() - 1);
        for (std::uint32_t v = 0; v < kKeySpace; ++v)
        {
            values_[next[f_function(static_cast<std::uint16_t>(v), 0)]++] = static_cast<std::uint16_t>(v);
        }
    }

    std::span<const std::uint16_t> preimages(std::uint16_t output) const
    {
        return std::span(values_).subspan(first_[output], first_[output + 1U] - first_[output]);
    }

  private:
    std::vector<std::uint32_t> first_ = std::vector<std::uint32_t>(kKeySpace + 1);
    std::vector<std::uint16_t> values_ = std::vector<std::uint16_t>(kKeySpace);
};

// One pair's constraint on a middle-round key: f_function(input, key) == output.
struct Equation
{
    std::uint16_t input;
    std::uint16_t output;
};

// The key satisfying the most equations, the lowest on a tie. It must satisfy
// more than half of them: a wrong k1 or k4 scatters the votes.
std::expected<std::uint16_t, Failure> majority_key(std::span<const Equation> equations, const FunctionInverse& inverse)
{
    std::vector<std::size_t> votes(kKeySpace);
    for (const Equation& equation : equations)
    {
        for (const std::uint16_t v : inverse.preimages(equation.output))
        {
            ++votes[equation.input ^ v];
        }
    }
    const auto winner = std::ranges::max_element(votes);
    if (*winner * 2 <= equations.size())
    {
        return std::unexpected(Failure::NoMatchingKey);
    }
    return static_cast<std::uint16_t>(winner - votes.begin());
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

std::expected<Recovery, Failure> recover_keys(bytes::ByteView plain, bytes::ByteView cipher)
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

    // The middle rounds: from each pair's halves after round 1 (x2) and
    // before round 4 (x3), f_function(x2, k2) == x3 ^ low(plain) and
    // f_function(x3, k3) == x2 ^ low(cipher).
    std::vector<Equation> k2_equations;
    std::vector<Equation> k3_equations;
    k2_equations.reserve(pairs.size());
    k3_equations.reserve(pairs.size());
    for (const WordPair& pair : pairs)
    {
        const auto x2 = static_cast<std::uint16_t>(high(pair.plain) ^ f_function(low(pair.plain), k1));
        const auto x3 = static_cast<std::uint16_t>(high(pair.cipher) ^ f_function(low(pair.cipher), k4));
        k2_equations.push_back({x2, static_cast<std::uint16_t>(x3 ^ low(pair.plain))});
        k3_equations.push_back({x3, static_cast<std::uint16_t>(x2 ^ low(pair.cipher))});
    }
    const FunctionInverse inverse;
    const auto k2 = majority_key(k2_equations, inverse);
    if (!k2.has_value())
    {
        return std::unexpected(k2.error());
    }
    const auto k3 = majority_key(k3_equations, inverse);
    if (!k3.has_value())
    {
        return std::unexpected(k3.error());
    }

    const Keys keys{k1, *k2, *k3, k4};
    const auto reproduced = static_cast<std::size_t>(std::ranges::count_if(
        pairs, [&keys](const WordPair& pair) { return encrypt(pair.plain, keys) == pair.cipher; }));
    return Recovery{.keys = keys, .distinct_pairs = pairs.size(), .reproduced_pairs = reproduced};
}

} // namespace subaru_key_recovery
