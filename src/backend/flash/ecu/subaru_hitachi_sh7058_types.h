#pragma once

#include <cstdint>

namespace fastecu::flash
{
struct SubaruHitachiSh7058KlinePlan
{
    int initial_baud = 4800;
    std::uint8_t tester_id = 0xf0;
    std::uint8_t target_id = 0x10;
    std::uint32_t page_size = 0x80;
};

struct SubaruHitachiSh7058CanPlan
{
    int bitrate = 500000;
    std::uint32_t request_id = 0x7e0;
    std::uint32_t response_id = 0x7e8;
    bool extended_id = false;
    std::uint32_t frame_size = 0x100;
};
} // namespace fastecu::flash
