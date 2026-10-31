#pragma once
#include <cstdint>

#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// Legacy: flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.{h,cpp}. Jumps to
// the TCU's resident on-board kernel via SecurityAccess + 0x10/0x42, the
// same shape as the sibling SubaruTcuCvtMitsuMh8111CanPlan family. Unlike
// MH8111, MH8104's read window and its sole flashed block coincide exactly
// ({0x8000, 0x78000} for both) -- see
// subaru_tcu_cvt_mitsu_mh8104_can_plan.cpp's kReadRegion/kWriteRegion
// comment. This family's defining, deliberate-to-preserve quirk is that
// every response-content check in connect_bootloader/read_mem/reflash_block
// after the initial kernel-alive probe is commented out in legacy
// (`// return STATUS_ERROR;`) -- it tolerates any ECU response content and
// only a genuine transport-level failure (timeout/disconnect/cancellation)
// stops it. See subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp for the full
// account.
struct SubaruTcuCvtMitsuMh8104CanPlan
{
    std::uint32_t request_id;  // 0x7e1
    std::uint32_t response_id; // 0x7e9
    int bitrate;               // 500000
    bool extended_id;          // false
};

// Requires no kernel: Jumps to the TCU's resident on-board kernel via
// SecurityAccess + 0x10/0x42, uploading no image -- the same shape as
// SubaruTcuCvtMitsuMh8111CanPlan. Unlike MH8111, every response-content
// check in legacy after the kernel-alive probe is commented out
// (`// return STATUS_ERROR;`); this family tolerates any ECU response
// content and only a transport-level failure stops it.
template <> struct FamilyTraits<SubaruTcuCvtMitsuMh8104CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruTcuCvtMitsuMh8104Can;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
};

template <> struct PlanValidator<SubaruTcuCvtMitsuMh8104CanPlan>
{
    static Status Validate(const FlashPlan& plan);
};
} // namespace fastecu::flash
