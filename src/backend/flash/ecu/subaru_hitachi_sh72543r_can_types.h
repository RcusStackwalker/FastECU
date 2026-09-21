#pragma once
#include <cstdint>

namespace fastecu::flash
{
// Legacy SH72543R CAN: 2 MiB read; only [0x6000, 0x200000) is programmed.
// TestWrite is rejected: legacy reflash_block ignores its test_write_arg.
struct SubaruHitachiSh72543rCanPlan
{
    std::uint32_t request_id;
    std::uint32_t response_id;
    int bitrate;
    bool extended_id;
    std::uint32_t page_size;
    std::uint32_t write_frame_size;
};
} // namespace fastecu::flash
