#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/protocol/idiagnostic_link.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// OBD-II framing rules the DTC dialog used, reproduced exactly (step 6g spec,
// "DTC today"). Byte comparisons are unsigned; bounds are checked.
namespace fastecu::diagnostics
{

enum class ObdProtocol
{
    kIso9141,
    kIso14230,
    kIso15765,
};

constexpr std::string_view protocol_name(ObdProtocol protocol) noexcept
{
    switch (protocol)
    {
    case ObdProtocol::kIso9141:
        return "iso9141";
    case ObdProtocol::kIso14230:
        return "iso14230";
    case ObdProtocol::kIso15765:
        return "iso15765";
    }
    return "iso9141";
}

// Index of the service-response byte in a received frame.
constexpr std::size_t response_index(ObdProtocol protocol) noexcept
{
    return protocol == ObdProtocol::kIso15765 ? 4 : 3;
}

bytes::Bytes build_request(ObdProtocol protocol, std::uint32_t source_id, bytes::ByteView payload);

enum class ResponseCheck
{
    kShort, // no response byte at response_index(); the caller keeps reading
    kOk,
    kNrc,     // 0x7F at response_index()
    kWrongId, // mode or PID echo does not match, or the PID echo is missing
};

ResponseCheck check_response(ObdProtocol protocol, bytes::ByteView frame, std::uint8_t mode,
                             std::optional<std::uint8_t> pid);

bytes::Bytes unframe_data_response(ObdProtocol protocol, bytes::ByteView frame);
bytes::Bytes unframe_dtc_list_response(ObdProtocol protocol, bytes::ByteView frame);

// The header to set after a five-baud init, or nullopt when rejected.
std::optional<KlineHeader> five_baud_header(ObdProtocol requested, bytes::ByteView response, bool uses_j2534);
bool fast_init_accepted(bytes::ByteView response);

// "%02x " per byte, as the dialog's parse_message_to_hex did.
std::string format_hex(bytes::ByteView data);
std::string format_pid_page_label(std::size_t page, bytes::ByteView bitmap);
std::string format_supported_pids(std::size_t page, bytes::ByteView bitmap);
std::vector<std::uint16_t> decode_dtcs(bytes::ByteView data);

} // namespace fastecu::diagnostics
