#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_hitachi_m32r_can_operation.{h,cpp}. Single
// protocol variant, no vendor challenge, no capacity choice -- unlike
// MitsuColtM32rCanPlan this struct carries no operation-shaping fields, only
// the CAN identity every exchange needs.
struct SubaruHitachiM32rCanPlan
{
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false
};

} // namespace fastecu::flash
