#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/bytes_views.h"

#include <array>
#include <bit>

namespace ssm_protocol
{

namespace
{

template <bool Reverse, std::size_t Rounds>
uint32_t TransformWord(uint32_t word, std::span<const std::uint16_t, Rounds> keytogenerateindex,
                       IndexTransformation indextransformation)
{
    auto lo = static_cast<std::uint16_t>(word);
    auto hi = static_cast<std::uint16_t>(word >> 16U);
    for (std::size_t r = 0; r < Rounds; ++r)
    {
        const std::uint32_t v = static_cast<std::uint16_t>(lo ^ keytogenerateindex[Reverse ? (Rounds - 1 - r) : r]);
        const std::uint32_t index = v | (v << 16U);

        const auto encryptionkey = static_cast<std::uint16_t>(
            indextransformation[index & 0x1FU] + (indextransformation[(index >> 4U) & 0x1FU] << 4U) +
            (indextransformation[(index >> 8U) & 0x1FU] << 8U) + (indextransformation[(index >> 12U) & 0x1FU] << 12U));

        const auto next = static_cast<std::uint16_t>(std::rotr(encryptionkey, 3) ^ hi);
        hi = lo;
        lo = next;
    }

    return static_cast<std::uint32_t>(hi) | (static_cast<std::uint32_t>(lo) << 16U);
}

// Two index-transformation lookups folded into one: entry i is
// t[i & 0x1F] + (t[(i >> 4) & 0x1F] << 4) for a 9-bit i. A round then needs two
// loads instead of four. Building the table costs about as much as transforming
// 35 words, so only multi-word payloads use it.
using PairTable = std::array<std::uint16_t, 512>;

PairTable BuildPairTable(IndexTransformation indextransformation)
{
    PairTable pairs;
    for (std::size_t i = 0; i < pairs.size(); ++i)
    {
        pairs[i] =
            static_cast<uint16_t>(indextransformation[i & 0x1FU] + (indextransformation[(i >> 4U) & 0x1FU] << 4U));
    }
    return pairs;
}

uint32_t TransformWordPaired(uint32_t word, KeyToGenerateIndex keytogenerateindex, const PairTable& pairs)
{
    auto lo = static_cast<std::uint16_t>(word);
    auto hi = static_cast<std::uint16_t>(word >> 16U);
    for (const std::uint16_t key : keytogenerateindex)
    {
        const std::uint32_t v = static_cast<std::uint16_t>(lo ^ key);
        const std::uint32_t index = v | (v << 16U);

        const auto encryptionkey =
            static_cast<std::uint16_t>(pairs[index & 0x1FFU] + (pairs[(index >> 8U) & 0x1FFU] << 8U));

        const auto next = static_cast<std::uint16_t>(std::rotr(encryptionkey, 3) ^ hi);
        hi = lo;
        lo = next;
    }

    return static_cast<std::uint32_t>(hi) | (static_cast<std::uint32_t>(lo) << 16U);
}

constexpr std::size_t kPairTableMinWords = 64;

} // namespace

bytes::Bytes CalculateSeedKey(bytes::ByteView seed, SeedKeyToGenerateIndex keytogenerateindex,
                              IndexTransformation indextransformation)
{
    bytes::Bytes key;
    if (seed.size() < 4)
    {
        return key;
    }

    bytes::AppendU32Be(key, TransformWord<true>(bytes::ReadU32Be(seed, 0), keytogenerateindex, indextransformation));
    return key;
}

bytes::Bytes CalculatePayload(bytes::ByteView buf, KeyToGenerateIndex keytogenerateindex,
                              IndexTransformation indextransformation)
{
    const auto words = buf | bytes::views::kU32Be;
    if (words.size() >= kPairTableMinWords)
    {
        const PairTable pairs = BuildPairTable(indextransformation);
        return bytes::ComposeBe(
            words | std::views::transform([&](std::uint32_t word)
                                          { return TransformWordPaired(word, keytogenerateindex, pairs); }));
    }
    return bytes::ComposeBe(
        words | std::views::transform([&](std::uint32_t word)
                                      { return TransformWord<false>(word, keytogenerateindex, indextransformation); }));
}

bytes::Bytes AddHeader(bytes::ByteView output, bytes::Byte tester_id, bytes::Byte target_id)
{
    using namespace bytes::literals;
    return bytes::ComposeBeWithChecksum(bytes::Sum8, 0x80_b, target_id, tester_id, bytes::Byte(output.size()), output);
}

bool HasValidFrame(bytes::ByteView frame, bytes::Byte receiver_id, bytes::Byte sender_id)
{
    constexpr std::size_t kHeaderLength = 4;
    constexpr std::size_t kChecksumLength = 1;
    if (frame.size() < kHeaderLength + kChecksumLength)
    {
        return false;
    }

    if (const std::size_t payload_length = frame[3]; frame.size() != kHeaderLength + payload_length + kChecksumLength)
    {
        return false;
    }

    if (frame[0] != 0x80 || frame[1] != receiver_id || frame[2] != sender_id)
    {
        return false;
    }

    return bytes::Sum8(frame.first(frame.size() - kChecksumLength)) == frame[frame.size() - kChecksumLength];
}

bool HasPayloadPrefix(bytes::ByteView frame, bytes::ByteView prefix, bytes::Byte receiver_id, bytes::Byte sender_id)
{
    if (!HasValidFrame(frame, receiver_id, sender_id))
    {
        return false;
    }

    if (const std::size_t payload_length = frame[3]; prefix.size() > payload_length)
    {
        return false;
    }

    return std::equal(prefix.begin(), prefix.end(), frame.begin() + 4);
}

} // namespace ssm_protocol
