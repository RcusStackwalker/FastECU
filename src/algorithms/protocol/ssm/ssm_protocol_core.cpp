#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include "src/algorithms/protocol/bytes_compose.h"

#include <algorithm>

namespace ssm_protocol
{

namespace
{

template <std::size_t Rounds>
uint32_t TransformWord(uint32_t word, std::span<const std::uint16_t, Rounds> keytogenerateindex,
                       IndexTransformation indextransformation, bool reverse)
{
    for (std::size_t r = 0; r < Rounds; ++r)
    {
        const std::size_t ki = reverse ? (Rounds - 1 - r) : r;
        const uint16_t wordtogenerateindex = word;
        const uint16_t wordtobeencrypted = word >> 16U;
        uint32_t index = wordtogenerateindex ^ keytogenerateindex[ki];
        index += index << 16U;

        uint16_t encryptionkey = 0;
        for (unsigned int n = 0; n < 4; ++n)
        {
            encryptionkey += indextransformation[(index >> (n * 4)) & 0x1FU] << (n * 4);
        }

        encryptionkey = (encryptionkey >> 3U) + (encryptionkey << 13U);
        word = (encryptionkey ^ wordtobeencrypted) + (wordtogenerateindex << 16U);
    }

    return (word >> 16U) + (word << 16U);
}

} // namespace

bytes::Bytes CalculateSeedKey(bytes::ByteView seed, SeedKeyToGenerateIndex keytogenerateindex,
                              IndexTransformation indextransformation)
{
    bytes::Bytes key;
    if (seed.size() < 4)
    {
        return key;
    }

    bytes::AppendU32Be(key, TransformWord(bytes::ReadU32Be(seed, 0), keytogenerateindex, indextransformation, true));
    return key;
}

bytes::Bytes CalculatePayload(bytes::ByteView buf, uint32_t len, KeyToGenerateIndex keytogenerateindex,
                              IndexTransformation indextransformation)
{
    bytes::Bytes encrypted;
    if (buf.empty() || len == 0)
    {
        return encrypted;
    }

    len &= ~uint32_t(3);
    if (len > uint32_t(buf.size()))
    {
        len = uint32_t(buf.size()) & ~uint32_t(3);
    }

    for (uint32_t i = 0; i < len; i += 4)
    {
        bytes::AppendU32Be(encrypted,
                           TransformWord(bytes::ReadU32Be(buf, i), keytogenerateindex, indextransformation, false));
    }

    return encrypted;
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
