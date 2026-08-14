#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include "src/algorithms/protocol/bytes_compose.h"

#include <algorithm>

namespace
{

uint32_t transformWord(uint32_t word, const uint16_t *keytogenerateindex,
                       const uint8_t *indextransformation, int rounds, bool reverse)
{
    for (int r = 0; r < rounds; ++r)
    {
        const int ki = reverse ? (rounds - 1 - r) : r;
        const uint16_t wordtogenerateindex = word;
        const uint16_t wordtobeencrypted = word >> 16;
        uint32_t index = wordtogenerateindex ^ keytogenerateindex[ki];
        index += index << 16;

        uint16_t encryptionkey = 0;
        for (int n = 0; n < 4; ++n)
        {
            encryptionkey += indextransformation[(index >> (n * 4)) & 0x1F] << (n * 4);
        }

        encryptionkey = (encryptionkey >> 3) + (encryptionkey << 13);
        word = (encryptionkey ^ wordtobeencrypted) + (wordtogenerateindex << 16);
    }

    return (word >> 16) + (word << 16);
}

} // namespace

namespace SsmProtocol
{

bytes::Bytes calculateSeedKey(bytes::ByteView seed, const uint16_t *keytogenerateindex,
                              const uint8_t *indextransformation)
{
    bytes::Bytes key;
    if (seed.size() < 4 || keytogenerateindex == nullptr || indextransformation == nullptr)
    {
        return key;
    }

    bytes::appendU32Be(key, transformWord(bytes::readU32Be(seed, 0), keytogenerateindex,
                                          indextransformation, 16, true));
    return key;
}

bytes::Bytes calculatePayload(bytes::ByteView buf, uint32_t len,
                              const uint16_t *keytogenerateindex,
                              const uint8_t *indextransformation)
{
    bytes::Bytes encrypted;
    if (buf.empty() || len == 0 || keytogenerateindex == nullptr || indextransformation == nullptr)
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
        bytes::appendU32Be(encrypted,
                           transformWord(bytes::readU32Be(buf, i), keytogenerateindex,
                                         indextransformation, 4, false));
    }

    return encrypted;
}

bytes::Bytes addHeader(bytes::ByteView output, bytes::Byte testerId, bytes::Byte targetId)
{
    using namespace bytes::literals;
    return bytes::composeBeWithChecksum(bytes::sum8, 0x80_b, targetId, testerId,
                                        bytes::Byte(output.size()), output);
}

bool hasValidFrame(bytes::ByteView frame, bytes::Byte receiverId, bytes::Byte senderId)
{
    constexpr std::size_t headerLength = 4;
    constexpr std::size_t checksumLength = 1;
    if (frame.size() < headerLength + checksumLength)
    {
        return false;
    }

    const std::size_t payloadLength = frame[3];
    if (frame.size() != headerLength + payloadLength + checksumLength)
    {
        return false;
    }

    if (frame[0] != 0x80 || frame[1] != receiverId || frame[2] != senderId)
    {
        return false;
    }

    return bytes::sum8(frame.first(frame.size() - checksumLength)) ==
           frame[frame.size() - checksumLength];
}

bool hasPayloadPrefix(bytes::ByteView frame, bytes::ByteView prefix,
                      bytes::Byte receiverId, bytes::Byte senderId)
{
    if (!hasValidFrame(frame, receiverId, senderId))
    {
        return false;
    }

    const std::size_t payloadLength = frame[3];
    if (prefix.size() > payloadLength)
    {
        return false;
    }

    return std::equal(prefix.begin(), prefix.end(), frame.begin() + 4);
}

} // namespace SsmProtocol
