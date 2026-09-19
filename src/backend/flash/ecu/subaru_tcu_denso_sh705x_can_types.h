#pragma once

#include <cstdint>

namespace fastecu::flash
{

// Wire setup for the ISO-15765-only Subaru Denso SH705x TCU family.
struct SubaruTcuDensoSh705xCanPlan
{
    std::uint32_t request_id{0x7E1};
    std::uint32_t response_id{0x7E9};
    int bitrate{500000};
    bool extended_id{false};
};

} // namespace fastecu::flash
