#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_denso_sh72543_can_diesel_operation.{h,cpp}. Region
// fields carry fblocks_SH72543d[0] exactly -- what execute() passes to
// read_memory (line 74) and what read_memory's 0x34/0x35 setup PDUs then
// compute from those arguments (lines 826-842, 865-881). Unlike its three
// siblings this family does not overwrite the arguments: that hardcode is
// commented out at lines 813-814.
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
