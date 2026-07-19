#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"

namespace MitsuColtCanCdbg
{

CdbgFrame buildInitFrame()
{
    return CdbgFrame{kCmdInit, 1, 0, 0, 0, 0, 0, 0};
}

CdbgFrame buildSecuritySeedRequestFrame()
{
    return CdbgFrame{kCmdSecuritySeed, 0, kSecurityLogAccess, 0, 0, 0, 0, 0};
}

std::uint32_t seedToKey(std::uint32_t seed)
{
    bytes::Byte data[4] = {};
    bytes::writeU32Be(data, 0, seed);

    for (int i = 0; i < 4; ++i)
    {
        bytes::Byte x = data[i];
        switch (x & 0x03)
        {
        case 0:
            x = static_cast<bytes::Byte>(x + 145);
            break;
        case 1:
            x = static_cast<bytes::Byte>(x + 24);
            break;
        case 2:
            x = static_cast<bytes::Byte>(x + 211);
            break;
        case 3:
            x = static_cast<bytes::Byte>(x + 2);
            break;
        }
        data[i] = static_cast<bytes::Byte>((x << 3) | (x >> 5)); // 8-bit rotate-left by 3
    }

    int parity = (data[0] & 1) + (data[1] & 1) + (data[2] & 1) + (data[3] & 1);
    bytes::Byte n[4];
    switch (parity)
    {
    case 0:
        n[0] = data[1];
        n[1] = data[3];
        n[2] = data[2];
        n[3] = data[0];
        break;
    case 1:
        n[0] = data[3];
        n[1] = data[2];
        n[2] = data[0];
        n[3] = data[1];
        break;
    case 2:
        n[0] = data[1];
        n[1] = data[2];
        n[2] = data[3];
        n[3] = data[0];
        break;
    case 3:
        n[0] = data[1];
        n[1] = data[0];
        n[2] = data[2];
        n[3] = data[3];
        break;
    default:
        n[0] = data[2];
        n[1] = data[0];
        n[2] = data[1];
        n[3] = data[3];
        break;
    }

    std::uint16_t word0 = static_cast<std::uint16_t>(((n[0] << 8) + n[1]) * 3 + n[3] * 8);
    std::uint16_t word1 = static_cast<std::uint16_t>(((n[2] << 8) + n[3]) * 5 + n[1] * 8);

    return (std::uint32_t(word0 >> 8) << 24) | (std::uint32_t(word0 & 0xFF) << 16) | (std::uint32_t(word1 >> 8) << 8) | std::uint32_t(word1 & 0xFF);
}

std::uint32_t extractSeed(bytes::ByteView reply)
{
    if (reply.size() < 8)
    {
        return 0;
    }
    return bytes::readU32Be(reply, 4);
}

CdbgFrame buildSecurityKeyFrame(std::uint32_t key)
{
    CdbgFrame frame{kCmdSecurityKey, 0, 0, 0, 0, 0, 0, 0};
    bytes::writeU32Be(frame, 2, key);
    return frame;
}

bool securityGranted(bytes::ByteView reply)
{
    if (reply.size() < 4)
    {
        return false;
    }
    return reply[3] != 0;
}

CdbgFrame buildLogResetFrame(bytes::Byte instance)
{
    return CdbgFrame{kCmdLogReset, 0, instance, 0, 0, 0, 0x06, 0x31};
}

CdbgFrame buildLogStartFrame(bytes::Byte instance, bytes::Byte frameCount, std::uint32_t intervalMs)
{
    bytes::Byte unitFlag;
    std::uint16_t encoded;
    if (intervalMs > 65535)
    {
        unitFlag = 1;
        encoded = static_cast<std::uint16_t>(intervalMs / 10);
    }
    else
    {
        unitFlag = 0;
        encoded = static_cast<std::uint16_t>(intervalMs);
    }

    CdbgFrame frame{kCmdLogStart, 0, 1, instance, frameCount, unitFlag, 0, 0};
    bytes::writeU16Be(frame, 6, encoded);
    return frame;
}

bool batchChannelsIntoFrames(const QVector<CdbgChannel>& channels,
                             QVector<QVector<CdbgChannel>>& outFrames)
{
    if (channels.isEmpty())
    {
        return false;
    }

    QVector<QVector<CdbgChannel>> frames;
    QVector<CdbgChannel> current;
    int byteIndex = 1;

    for (const CdbgChannel& ch : channels)
    {
        if (byteIndex + ch.size > 8)
        {
            frames.append(current);
            current.clear();
            byteIndex = 1;
        }
        current.append(ch);
        byteIndex += ch.size;
    }
    if (!current.isEmpty())
    {
        frames.append(current);
    }

    if (frames.size() > kMaxFrames)
    {
        return false;
    }

    outFrames = frames;
    return true;
}

std::vector<CdbgFrame> buildFrameInitFrames(bytes::Byte instance, bytes::Byte frameIndex,
                                            const QVector<CdbgChannel>& frameItems)
{
    std::vector<CdbgFrame> out;
    out.reserve(static_cast<std::size_t>(frameItems.size() * 2));
    for (int i = 0; i < frameItems.size(); ++i)
    {
        out.push_back(CdbgFrame{
            kCmdLogSelectItem,
            0,
            instance,
            frameIndex,
            static_cast<bytes::Byte>(i),
            0,
            0,
            0});

        const CdbgChannel& ch = frameItems.at(i);
        CdbgFrame pointerFrame{kCmdLogSetPointer, 0, ch.size, 0, 0, 0, 0, 0};
        bytes::writeU32Be(pointerFrame, 4, ch.pointer);
        out.push_back(pointerFrame);
    }
    return out;
}

QVector<std::uint32_t> decodeFrame(bytes::Byte expectedFrameIndex,
                                   const QVector<CdbgChannel>& frameItems,
                                   bytes::ByteView frame)
{
    if (frame.empty() || frame[0] != expectedFrameIndex)
    {
        return {};
    }

    int need = 1;
    for (const CdbgChannel& ch : frameItems)
    {
        need += ch.size;
    }
    if (frame.size() < need)
    {
        return {};
    }

    QVector<std::uint32_t> out;
    int offset = 1;
    for (const CdbgChannel& ch : frameItems)
    {
        std::uint32_t value = 0;
        switch (ch.size)
        {
        case 1:
            value = frame[static_cast<std::size_t>(offset)];
            break;
        case 2:
            value = bytes::readU16Be(frame, static_cast<std::size_t>(offset));
            break;
        case 4:
            value = bytes::readU32Be(frame, static_cast<std::size_t>(offset));
            break;
        default:
            for (int k = 0; k < ch.size; ++k)
            {
                value = (value << 8) | std::uint32_t(frame[static_cast<std::size_t>(offset + k)]);
            }
            break;
        }
        out.append(value);
        offset += ch.size;
    }
    return out;
}

} // namespace MitsuColtCanCdbg
