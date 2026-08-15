#pragma once
#include <cstdint>

namespace fastecu::flash
{
// Legacy: flash_tcu_cvt_subaru_hitachi_m32r_can_operation.{h,cpp}. execute()
// calls the dead hack_words() (always STATUS_ERROR); this plan/executor pair
// ports the real, previously-unreachable connect_bootloader/read_mem/
// write_mem logic instead -- see the wave-3 design's "Deliberate divergence"
// section.
struct SubaruTcuCvtHitachiM32rCanPlan
{
    std::uint32_t request_id;  // 0x7e1
    std::uint32_t response_id; // 0x7e9
    int bitrate;               // 500000
    bool extended_id;          // false
};
} // namespace fastecu::flash
