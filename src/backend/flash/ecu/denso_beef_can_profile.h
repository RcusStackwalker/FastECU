#pragma once

#include <chrono>
#include <string_view>

namespace fastecu::flash
{

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
    BeefPduFormat pdu_format;
    std::chrono::milliseconds init_timeout{500}; // read timeout of the flash-init exchanges
    bool upper_hex_sizes{false};                 // log the kernel's buffer sizes in upper-case hex
};

} // namespace fastecu::flash
