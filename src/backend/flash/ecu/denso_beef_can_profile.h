#pragma once

#include <string_view>

namespace fastecu::flash
{

// What a tolerant query logs after "Wrong response from {device}" when the
// channel rejects a reply.
enum class BeefWrongReply
{
    kBare,
    kNotValidAnswer,
    kErrorDetail,
};

// How a wrong reply's PDU is rendered in the log.
enum class BeefPduFormat
{
    kHex,
    kDescribe,
};

// The per-family wording of a BEEF CAN session. Pure data: one constexpr per
// family, no hooks.
struct DensoBeefCanProfile
{
    std::string_view device; // "ECU" or "TCU"
    std::string_view family; // names the executor in errors: "petrol", "diesel", "TCU"
    BeefWrongReply wrong_reply;
    BeefPduFormat pdu_format;
};

} // namespace fastecu::flash
