#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"
#include <array>

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
    std::array<bytes::Byte, 4> data = {};
    bytes::writeU32Be(data, 0, seed);

    for (int i = 0; i < 4; ++i)
    {
        bytes::Byte x = data[i];
        switch (x & 0x03U)
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
        data[i] = static_cast<bytes::Byte>((static_cast<unsigned>(x) << 3U) |
                                           (static_cast<unsigned>(x) >> 5U)); // 8-bit rotate-left by 3
    }

    int parity = (data[0] & 1U) + (data[1] & 1U) + (data[2] & 1U) + (data[3] & 1U);
    std::array<bytes::Byte, 4> n{};
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

    std::uint16_t word0 = static_cast<std::uint16_t>(((n[0] << 8U) + n[1]) * 3 + n[3] * 8);
    std::uint16_t word1 = static_cast<std::uint16_t>(((n[2] << 8U) + n[3]) * 5 + n[1] * 8);

    return (std::uint32_t(word0 >> 8U) << 24U) | (std::uint32_t(word0 & 0xFFU) << 16U) |
           (std::uint32_t(word1 >> 8U) << 8U) | std::uint32_t(word1 & 0xFFU);
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

bool batchChannelsIntoFrames(const std::vector<CdbgChannel>& channels, std::vector<std::vector<CdbgChannel>>& outFrames)
{
    if (channels.empty())
    {
        return false;
    }

    std::vector<std::vector<CdbgChannel>> frames;
    std::vector<CdbgChannel> current;
    int byteIndex = 1;

    for (const CdbgChannel& ch : channels)
    {
        if (byteIndex + ch.size > 8)
        {
            frames.push_back(current);
            current.clear();
            byteIndex = 1;
        }
        current.push_back(ch);
        byteIndex += ch.size;
    }
    if (!current.empty())
    {
        frames.push_back(current);
    }

    if (frames.size() > static_cast<std::size_t>(kMaxFrames))
    {
        return false;
    }

    outFrames = frames;
    return true;
}

std::vector<CdbgFrame> buildFrameInitFrames(bytes::Byte instance, bytes::Byte frameIndex,
                                            const std::vector<CdbgChannel>& frameItems)
{
    std::vector<CdbgFrame> out;
    out.reserve(frameItems.size() * 2);
    for (std::size_t i = 0; i < frameItems.size(); ++i)
    {
        out.push_back(CdbgFrame{kCmdLogSelectItem, 0, instance, frameIndex, static_cast<bytes::Byte>(i), 0, 0, 0});

        const CdbgChannel& ch = frameItems.at(i);
        CdbgFrame pointerFrame{kCmdLogSetPointer, 0, ch.size, 0, 0, 0, 0, 0};
        bytes::writeU32Be(pointerFrame, 4, ch.pointer);
        out.push_back(pointerFrame);
    }
    return out;
}

std::vector<std::uint32_t> decodeFrame(bytes::Byte expectedFrameIndex, const std::vector<CdbgChannel>& frameItems,
                                       bytes::ByteView frame)
{
    if (frame.empty() || frame[0] != expectedFrameIndex)
    {
        return {};
    }

    std::size_t need = 1;
    for (const CdbgChannel& ch : frameItems)
    {
        need += ch.size;
    }
    if (frame.size() < need)
    {
        return {};
    }

    std::vector<std::uint32_t> out;
    int offset = 1;
    for (const CdbgChannel& ch : frameItems)
    {
        out.push_back(bytes::readUBe(frame, static_cast<std::size_t>(offset), ch.size));
        offset += ch.size;
    }
    return out;
}

} // namespace MitsuColtCanCdbg
