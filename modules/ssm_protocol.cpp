#include "modules/ssm_protocol.h"

#include "src/algorithms/protocol/qt_bytes.h"

#include <array>
#include <algorithm>
#include <cstdio>

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

const std::array<uint32_t, 256>& crcTable()
{
    static const std::array<uint32_t, 256> table = []
    {
        std::array<uint32_t, 256> t = {};
        constexpr uint32_t polynomial = 0x5AA5A55A;

        for (uint32_t i = 0; i < t.size(); ++i)
        {
            uint32_t crc = 0;
            uint32_t c = i;

            for (uint32_t j = 0; j < 8; ++j)
            {
                if ((crc ^ c) & 0x00000001U)
                {
                    crc = (crc >> 1) ^ polynomial;
                }
                else
                {
                    crc = crc >> 1;
                }
                c = c >> 1;
            }
            t[i] = crc;
        }

        return t;
    }();

    return table;
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

bytes::Byte checksum(bytes::ByteView output, bool dec0x100)
{
    bytes::Byte value = 0;
    for (const bytes::Byte byte : output)
    {
        value += byte;
    }

    if (dec0x100)
    {
        value = bytes::Byte(0x100 - value);
    }

    return value;
}

bytes::Bytes addHeader(bytes::ByteView output, bytes::Byte testerId, bytes::Byte targetId,
                       bool dec0x100)
{
    bytes::Bytes framed;
    const auto length = bytes::Byte(output.size());

    framed.reserve(output.size() + 5);
    framed.push_back(0x80);
    framed.push_back(targetId);
    framed.push_back(testerId);
    framed.push_back(length);
    framed.insert(framed.end(), output.begin(), output.end());
    framed.push_back(checksum(framed, dec0x100));

    return framed;
}

bool hasValidFrame(bytes::ByteView frame, bytes::Byte receiverId, bytes::Byte senderId,
                   bool dec0x100)
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

    return checksum(frame.first(frame.size() - checksumLength), dec0x100) == frame[frame.size() - checksumLength];
}

bool hasPayloadPrefix(bytes::ByteView frame, bytes::ByteView prefix,
                      bytes::Byte receiverId, bytes::Byte senderId, bool dec0x100)
{
    if (!hasValidFrame(frame, receiverId, senderId, dec0x100))
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

std::string toHex(bytes::ByteView received)
{
    std::string msg;
    msg.reserve(received.size() * 3);
    char hex[4] = {};
    for (const bytes::Byte byte : received)
    {
        std::snprintf(hex, sizeof(hex), "%02x ", byte);
        msg.append(hex);
    }
    return msg;
}

uint32_t crc32(bytes::ByteView bytes)
{
    uint32_t crc = 0xFFFFFFFF;
    const auto& table = crcTable();
    for (const auto byte : bytes)
    {
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

QByteArray calculateSeedKey(const QByteArray& seed, const uint16_t *keytogenerateindex,
                            const uint8_t *indextransformation)
{
    return bytes::toQByteArray(calculateSeedKey(bytes::view(seed), keytogenerateindex,
                                                indextransformation));
}

QByteArray calculatePayload(const QByteArray& buf, uint32_t len,
                            const uint16_t *keytogenerateindex,
                            const uint8_t *indextransformation)
{
    return bytes::toQByteArray(calculatePayload(bytes::view(buf), len, keytogenerateindex,
                                                indextransformation));
}

uint8_t checksum(const QByteArray& output, bool dec0x100)
{
    return checksum(bytes::view(output), dec0x100);
}

QByteArray addHeader(const QByteArray& output, uint8_t testerId, uint8_t targetId, bool dec0x100)
{
    return bytes::toQByteArray(addHeader(bytes::view(output), testerId, targetId, dec0x100));
}

bool hasValidFrame(const QByteArray& frame, uint8_t receiverId, uint8_t senderId, bool dec0x100)
{
    return hasValidFrame(bytes::view(frame), receiverId, senderId, dec0x100);
}

bool hasPayloadPrefix(const QByteArray& frame, const QByteArray& prefix,
                      uint8_t receiverId, uint8_t senderId, bool dec0x100)
{
    return hasPayloadPrefix(bytes::view(frame), bytes::view(prefix), receiverId, senderId,
                            dec0x100);
}

QString toHex(const QByteArray& received)
{
    return QString::fromStdString(toHex(bytes::view(received)));
}

uint32_t crc32(const unsigned char *buf, uint32_t len)
{
    if (buf == nullptr)
    {
        return 0;
    }

    return crc32(bytes::ByteView(reinterpret_cast<const bytes::Byte *>(buf), len));
}

} // namespace SsmProtocol
