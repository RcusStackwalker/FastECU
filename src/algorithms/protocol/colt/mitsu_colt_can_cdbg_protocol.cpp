#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include <algorithm>
#include <array>
#include <bit>

namespace mitsu_colt_can_cdbg
{

using namespace bytes::literals;

CdbgFrame BuildInitFrame()
{
    return CdbgFrame{kCmdInit, 1, 0, 0, 0, 0, 0, 0};
}

CdbgFrame BuildSecuritySeedRequestFrame()
{
    return CdbgFrame{kCmdSecuritySeed, 0, kSecurityLogAccess, 0, 0, 0, 0, 0};
}

std::uint32_t SeedToKey(std::uint32_t seed)
{
    auto data = bytes::ComposeBe(seed);

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
        default:
            break;
        }
        data[i] = std::rotl(x, 3);
    }

    const auto parity = (data[0] & 1U) + (data[1] & 1U) + (data[2] & 1U) + (data[3] & 1U);
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

std::uint32_t ExtractSeed(bytes::ByteView reply)
{
    if (reply.size() < 8)
    {
        return 0;
    }
    return bytes::ReadU32Be(reply, 4);
}

CdbgFrame BuildSecurityKeyFrame(std::uint32_t key)
{
    return bytes::ComposeBe(kCmdSecurityKey, 0x00_b, key, 0x00_b, 0x00_b);
}

bool SecurityGranted(bytes::ByteView reply)
{
    if (reply.size() < 4)
    {
        return false;
    }
    return reply[3] != 0;
}

CdbgFrame BuildLogResetFrame(bytes::Byte instance)
{
    return CdbgFrame{kCmdLogReset, 0, instance, 0, 0, 0, 0x06, 0x31};
}

CdbgFrame BuildLogStartFrame(bytes::Byte instance, bytes::Byte frame_count, std::uint32_t interval_ms)
{
    bytes::Byte unit_flag;
    std::uint16_t encoded;
    if (interval_ms > 65535)
    {
        unit_flag = 1;
        encoded = static_cast<std::uint16_t>(interval_ms / 10);
    }
    else
    {
        unit_flag = 0;
        encoded = static_cast<std::uint16_t>(interval_ms);
    }

    return bytes::ComposeBe(kCmdLogStart, 0x00_b, 0x01_b, instance, frame_count, unit_flag, encoded);
}

bool BatchChannelsIntoFrames(const std::vector<CdbgChannel>& channels,
                             std::vector<std::vector<CdbgChannel>>& out_frames)
{
    if (channels.empty())
    {
        return false;
    }

    std::vector<std::vector<CdbgChannel>> frames;
    std::vector<CdbgChannel> current;
    int byte_index = 1;

    for (const CdbgChannel& ch : channels)
    {
        if (byte_index + ch.size > 8)
        {
            frames.push_back(current);
            current.clear();
            byte_index = 1;
        }
        current.push_back(ch);
        byte_index += ch.size;
    }
    if (!current.empty())
    {
        frames.push_back(current);
    }

    if (frames.size() > static_cast<std::size_t>(kMaxFrames))
    {
        return false;
    }

    out_frames = frames;
    return true;
}

std::vector<CdbgFrame> BuildFrameInitFrames(bytes::Byte instance, bytes::Byte frame_index,
                                            const std::vector<CdbgChannel>& frame_items)
{
    std::vector<CdbgFrame> out;
    out.reserve(frame_items.size() * 2);
    for (std::size_t i = 0; i < frame_items.size(); ++i)
    {
        out.push_back(CdbgFrame{kCmdLogSelectItem, 0, instance, frame_index, static_cast<bytes::Byte>(i), 0, 0, 0});

        const CdbgChannel& ch = frame_items.at(i);
        out.push_back(bytes::ComposeBe(kCmdLogSetPointer, 0x00_b, ch.size, 0x00_b, ch.pointer));
    }
    return out;
}

std::vector<std::uint32_t> DecodeFrame(bytes::Byte expected_frame_index, const std::vector<CdbgChannel>& frame_items,
                                       bytes::ByteView frame)
{
    if (frame.empty() || frame[0] != expected_frame_index)
    {
        return {};
    }

    const std::size_t need = std::ranges::fold_left(
        frame_items, std::size_t{1}, [](std::size_t sum, const CdbgChannel& ch) { return sum + ch.size; });
    if (frame.size() < need)
    {
        return {};
    }

    std::vector<std::uint32_t> out;
    int offset = 1;
    for (const CdbgChannel& ch : frame_items)
    {
        out.push_back(bytes::ReadUBe(frame, static_cast<std::size_t>(offset), ch.size));
        offset += ch.size;
    }
    return out;
}

} // namespace mitsu_colt_can_cdbg
