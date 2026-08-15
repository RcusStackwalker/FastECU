#pragma once
#include <cstdint>

namespace fastecu::flash
{
// Legacy: flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.{h,cpp}. Jumps to
// the TCU's resident on-board kernel via SecurityAccess + 0x10/0x42 (no
// kernel-alive pre-check shortcut, unlike the sibling Hitachi CAN TCU
// family -- connect_bootloader always runs its full sequence). Read and
// write windows deliberately do NOT overlap for this family -- see
// subaru_tcu_cvt_mitsu_mh8111_can_plan.cpp's kReadRegion/kWriteRegion
// comment.
struct SubaruTcuCvtMitsuMh8111CanPlan
{
    std::uint32_t request_id;  // 0x7e1
    std::uint32_t response_id; // 0x7e9
    int bitrate;               // 500000
    bool extended_id;          // false
};
} // namespace fastecu::flash
