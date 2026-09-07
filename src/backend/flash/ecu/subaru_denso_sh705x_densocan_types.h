#pragma once

#include <cstdint>

namespace fastecu::flash
{

// Wire and transport constants for the proprietary Subaru DensoCAN bootloader
// family. It begins and ends on 11-bit ISO-15765, with a 29-bit raw CAN
// bootloader transition only when a kernel upload is necessary.
struct SubaruDensoSh705xDensoCanPlan
{
    std::uint32_t iso_request_id{0x7E0};
    std::uint32_t iso_response_id{0x7E8};
    std::uint32_t raw_transmit_id{0x000FFFFE};
    std::uint32_t raw_receive_id{0x21};
    int bitrate{500000};
    bool iso_extended_id{false};
    bool raw_extended_id{true};
};

} // namespace fastecu::flash
