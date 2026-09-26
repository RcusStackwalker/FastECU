#include "src/backend/diagnostics/obd_frames.h"

#include <algorithm>
#include <array>
#include <format>

namespace fastecu::diagnostics
{

bytes::Bytes build_request(ObdProtocol protocol, std::uint32_t source_id, bytes::ByteView payload)
{
    bytes::Bytes out;
    if (protocol == ObdProtocol::Iso15765)
    {
        out = {0x00, 0x00, static_cast<bytes::Byte>((source_id >> 8U) & 0xFFU),
               static_cast<bytes::Byte>(source_id & 0xFFU)};
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

ResponseCheck check_response(ObdProtocol protocol, bytes::ByteView frame, std::uint8_t mode,
                             std::optional<std::uint8_t> pid)
{
    const std::size_t index = response_index(protocol);
    if (frame.size() <= index)
    {
        return ResponseCheck::Short;
    }
    if (frame[index] == 0x7F)
    {
        return ResponseCheck::Nrc;
    }
    if (frame[index] != static_cast<bytes::Byte>(mode | 0x40U))
    {
        return ResponseCheck::WrongId;
    }
    if (pid.has_value() && (frame.size() <= index + 1 || frame[index + 1] != *pid))
    {
        return ResponseCheck::WrongId;
    }
    return ResponseCheck::Ok;
}

namespace
{
bytes::Bytes tail(bytes::ByteView data, std::size_t from)
{
    if (from >= data.size())
    {
        return {};
    }
    return bytes::Bytes(data.begin() + static_cast<std::ptrdiff_t>(from), data.end());
}

// K-Line: drop the checksum, then keep only the last byte of a short frame.
bytes::ByteView without_checksum(bytes::ByteView frame)
{
    return frame.empty() ? frame : frame.first(frame.size() - 1);
}
} // namespace

bytes::Bytes unframe_data_response(ObdProtocol protocol, bytes::ByteView frame)
{
    if (protocol == ObdProtocol::Iso15765)
    {
        return tail(frame, response_index(protocol) + 3);
    }
    const bytes::ByteView body = without_checksum(frame);
    if (body.empty())
    {
        return {};
    }
    if (body.size() < 7)
    {
        return tail(body, body.size() - 1);
    }
    return tail(body, body.size() < 10 ? 5 : 6);
}

bytes::Bytes unframe_dtc_list_response(ObdProtocol protocol, bytes::ByteView frame)
{
    if (protocol == ObdProtocol::Iso15765)
    {
        return tail(frame, response_index(protocol) + 2);
    }
    const bytes::ByteView body = without_checksum(frame);
    if (body.empty())
    {
        return {};
    }
    if (body.size() < 7)
    {
        return tail(body, body.size() - 1);
    }
    return tail(body, 4);
}

std::optional<KlineHeader> five_baud_header(ObdProtocol requested, bytes::ByteView r, bool uses_j2534)
{
    if (uses_j2534)
    {
        if (requested == ObdProtocol::Iso9141 && r.size() > 7 && r[5] == '8' && r[7] == '8')
        {
            return KlineHeader::Iso9141;
        }
        if (requested == ObdProtocol::Iso14230 && r.size() > 9 && r[8] == '8' && r[9] == 'f')
        {
            return KlineHeader::Iso14230;
        }
        return std::nullopt;
    }
    if (r.size() > 2 && r[1] == 0x08 && r[2] == 0x08)
    {
        return KlineHeader::Iso9141;
    }
    if (r.size() > 2 && r[2] == 0x8F)
    {
        return KlineHeader::Iso14230;
    }
    return std::nullopt;
}

bool fast_init_accepted(bytes::ByteView response)
{
    static constexpr std::array<bytes::Byte, 6> kExpected{0x83, 0xF1, 0x10, 0xC1, 0xE9, 0x8F};
    return response.size() >= kExpected.size() && std::equal(kExpected.begin(), kExpected.end(), response.begin());
}

std::string format_hex(bytes::ByteView data)
{
    std::string out;
    for (const bytes::Byte b : data)
    {
        out += std::format("{:02x} ", b);
    }
    return out;
}

std::string format_pid_page_label(std::size_t page, bytes::ByteView bitmap)
{
    const std::size_t start = page * 0x20 + 1;
    return std::format("Supported PIDs 0x{:x}-0x{:x}: ", start, start + 0x1F) + format_hex(bitmap);
}

std::string format_supported_pids(std::size_t page, bytes::ByteView bitmap)
{
    const std::size_t start = page * 0x20 + 1;
    std::string out;
    for (std::size_t i = 0; i < bitmap.size(); ++i)
    {
        for (int j = 7; j >= 0; --j)
        {
            const std::size_t enabled = (static_cast<unsigned>(bitmap[i]) >> static_cast<unsigned>(j)) & 1U;
            out += std::format("0x{:02x} ", enabled * ((i * 8 + 7 - static_cast<std::size_t>(j)) + start));
        }
    }
    return out;
}

std::vector<std::uint16_t> decode_dtcs(bytes::ByteView data)
{
    std::vector<std::uint16_t> codes;
    for (std::size_t i = 0; i < data.size(); i += 2)
    {
        // readU16Be yields 0 for a trailing odd byte, which is dropped below.
        if (const std::uint16_t code = bytes::readU16Be(data, i); code != 0)
        {
            codes.push_back(code);
        }
    }
    std::ranges::sort(codes);
    return codes;
}

} // namespace fastecu::diagnostics
