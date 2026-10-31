#pragma once
#include <cstdint>

#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

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

// Requires no kernel: Jumps to the ECU's resident on-board kernel via
// SecurityAccess + 0x10/0x42, uploading no image.
template <> struct FamilyTraits<SubaruHitachiM32rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruHitachiM32rCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
};

template <> struct PlanValidator<SubaruHitachiM32rCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

} // namespace fastecu::flash
