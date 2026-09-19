#pragma once

#include <cstdint>

namespace fastecu::flash
{

struct SubaruDensoSh7058CanDieselPlan
{
    std::uint32_t request_id{0x7E0};
    std::uint32_t response_id{0x7E8};
    int bitrate{500000};
    bool extended_id{false};
};

} // namespace fastecu::flash
