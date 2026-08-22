#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_denso_1n83m_1_5m_can_operation.{h,cpp}. Single
// protocol variant. The region fields carry what legacy hardcoded in
// read_memory (lines 826-828) and the 0x34/0x35 setup PDUs (lines 842-852,
// 883-893): fblocks_N83M_1_5MB[1] exactly.
struct SubaruDenso1n83m_1_5mCanPlan
{
    std::uint32_t request_id;   // 0x7e0
    std::uint32_t response_id;  // 0x7e8
    int bitrate;                // 500000
    bool extended_id;           // false
    std::uint32_t lead_pad_len; // 0x10000, prepended to a read image as 0xFF
    std::uint32_t tail_pad_len; // 0x100, appended to a read image as 0xFF
};

} // namespace fastecu::flash
