#include "modules/ssm_protocol.h"

#include <array>

namespace {

uint32_t readBigEndianWord(const QByteArray &data, int offset)
{
    return ((uint32_t(uint8_t(data.at(offset))) << 24) & 0xFF000000U)
           | ((uint32_t(uint8_t(data.at(offset + 1))) << 16) & 0x00FF0000U)
           | ((uint32_t(uint8_t(data.at(offset + 2))) << 8) & 0x0000FF00U)
           | (uint32_t(uint8_t(data.at(offset + 3))) & 0x000000FFU);
}

uint32_t transformWord(uint32_t word, const uint16_t *keytogenerateindex,
                       const uint8_t *indextransformation, int rounds, bool reverse)
{
    for (int r = 0; r < rounds; ++r) {
        const int ki = reverse ? (rounds - 1 - r) : r;
        const uint16_t wordtogenerateindex = word;
        const uint16_t wordtobeencrypted = word >> 16;
        uint32_t index = wordtogenerateindex ^ keytogenerateindex[ki];
        index += index << 16;

        uint16_t encryptionkey = 0;
        for (int n = 0; n < 4; ++n) {
            encryptionkey += indextransformation[(index >> (n * 4)) & 0x1F] << (n * 4);
        }

        encryptionkey = (encryptionkey >> 3) + (encryptionkey << 13);
        word = (encryptionkey ^ wordtobeencrypted) + (wordtogenerateindex << 16);
    }

    return (word >> 16) + (word << 16);
}

void appendBigEndianWord(QByteArray *out, uint32_t word)
{
    out->append(uint8_t(word >> 24));
    out->append(uint8_t(word >> 16));
    out->append(uint8_t(word >> 8));
    out->append(uint8_t(word));
}

const std::array<uint32_t, 256> &crcTable()
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t = {};
        constexpr uint32_t polynomial = 0x5AA5A55A;

        for (uint32_t i = 0; i < t.size(); ++i) {
            uint32_t crc = 0;
            uint32_t c = i;

            for (uint32_t j = 0; j < 8; ++j) {
                if ((crc ^ c) & 0x00000001U) {
                    crc = (crc >> 1) ^ polynomial;
                } else {
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

namespace SsmProtocol {

QByteArray calculateSeedKey(const QByteArray &seed, const uint16_t *keytogenerateindex,
                            const uint8_t *indextransformation)
{
    QByteArray key;
    if (seed.size() < 4 || keytogenerateindex == nullptr || indextransformation == nullptr) {
        return key;
    }

    appendBigEndianWord(&key, transformWord(readBigEndianWord(seed, 0), keytogenerateindex,
                                            indextransformation, 16, true));
    return key;
}

QByteArray calculatePayload(const QByteArray &buf, uint32_t len,
                            const uint16_t *keytogenerateindex,
                            const uint8_t *indextransformation)
{
    QByteArray encrypted;
    if (buf.isEmpty() || len == 0 || keytogenerateindex == nullptr
        || indextransformation == nullptr) {
        return encrypted;
    }

    len &= ~uint32_t(3);
    if (len > uint32_t(buf.size())) {
        len = uint32_t(buf.size()) & ~uint32_t(3);
    }

    for (uint32_t i = 0; i < len; i += 4) {
        appendBigEndianWord(&encrypted,
                            transformWord(readBigEndianWord(buf, int(i)), keytogenerateindex,
                                          indextransformation, 4, false));
    }

    return encrypted;
}

uint8_t checksum(const QByteArray &output, bool dec0x100)
{
    uint8_t value = 0;
    for (int i = 0; i < output.length(); ++i) {
        value += uint8_t(output.at(i));
    }

    if (dec0x100) {
        value = uint8_t(0x100 - value);
    }

    return value;
}

QByteArray addHeader(QByteArray output, uint8_t testerId, uint8_t targetId, bool dec0x100)
{
    const uint8_t length = output.length();

    output.insert(0, uint8_t(0x80));
    output.insert(1, targetId & 0xFF);
    output.insert(2, testerId & 0xFF);
    output.insert(3, length);
    output.append(checksum(output, dec0x100));

    return output;
}

bool hasValidFrame(const QByteArray &frame, uint8_t receiverId, uint8_t senderId, bool dec0x100)
{
    constexpr int headerLength = 4;
    constexpr int checksumLength = 1;
    if (frame.size() < headerLength + checksumLength) {
        return false;
    }

    const auto valueAt = [&frame](int index) {
        return uint8_t(frame.at(index));
    };

    const int payloadLength = valueAt(3);
    if (frame.size() != headerLength + payloadLength + checksumLength) {
        return false;
    }

    if (valueAt(0) != 0x80 || valueAt(1) != receiverId || valueAt(2) != senderId) {
        return false;
    }

    return checksum(frame.left(frame.size() - checksumLength), dec0x100)
           == valueAt(frame.size() - checksumLength);
}

bool hasPayloadPrefix(const QByteArray &frame, const QByteArray &prefix,
                      uint8_t receiverId, uint8_t senderId, bool dec0x100)
{
    if (!hasValidFrame(frame, receiverId, senderId, dec0x100)) {
        return false;
    }

    const int payloadLength = uint8_t(frame.at(3));
    return prefix.size() <= payloadLength && frame.mid(4, prefix.size()) == prefix;
}

QString toHex(const QByteArray &received)
{
    QString msg;
    for (int i = 0; i < received.length(); ++i) {
        msg.append(QString("%1 ").arg(uint8_t(received.at(i)), 2, 16, QLatin1Char('0')).toUtf8());
    }
    return msg;
}

uint32_t crc32(const unsigned char *buf, uint32_t len)
{
    if (buf == nullptr) {
        return 0;
    }

    uint32_t crc = 0xFFFFFFFF;
    const auto &table = crcTable();
    while (len--) {
        crc = table[(crc ^ (*buf++)) & 0xff] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

} // namespace SsmProtocol
