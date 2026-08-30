#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_denso_sh72543_can_diesel_operation.{h,cpp}. Region
// fields carry what legacy hardcoded in read_memory (lines 828-830) and the
// 0x34/0x35 setup PDUs (lines 844-854, 881-891): fblocks_SH72543d[0] exactly.
struct SubaruDensoSh72543CanDieselPlan
{
    std::uint32_t request_id;   // 0x7e0
    std::uint32_t response_id;  // 0x7e8
    int bitrate;                // 500000
    bool extended_id;           // false
    std::uint32_t lead_pad_len; // 0x8000, prepended to a read image as 0xFF
    std::uint32_t tail_pad_len; // 0x100, appended to a read image as 0xFF
};

} // namespace fastecu::flash
