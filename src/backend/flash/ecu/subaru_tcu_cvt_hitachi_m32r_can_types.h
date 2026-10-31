#pragma once
#include <cstdint>

#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// Legacy: flash_tcu_cvt_subaru_hitachi_m32r_can_operation.{h,cpp}. execute()
// calls the dead hack_words() (always STATUS_ERROR); this plan/executor pair
// ports the real, previously-unreachable connect_bootloader/read_mem/
// write_mem logic instead -- see the original port design's "Deliberate divergence"
// section.
struct SubaruTcuCvtHitachiM32rCanPlan
{
    std::uint32_t request_id;  // 0x7e1
    std::uint32_t response_id; // 0x7e9
    int bitrate;               // 500000
    bool extended_id;          // false
};

// Requires no kernel: Jumps to the TCU's resident on-board kernel via
// SecurityAccess + 0x10/0x02, uploading no image.
template <> struct FamilyTraits<SubaruTcuCvtHitachiM32rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruTcuCvtHitachiM32rCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruTcuCvtHitachiM32rCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};
} // namespace fastecu::flash
