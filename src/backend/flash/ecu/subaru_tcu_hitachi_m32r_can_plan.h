#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// Legacy: flash_tcu_subaru_hitachi_m32r_can_operation.{h,cpp}.
//
// Deliberate divergence 1 (the safety-critical one -- see the plan builder):
// write_mem(bool test_write_arg) forwards test_write_arg to reflash_block
// (reflash_block's signature is the only other occurrence of the name in the
// file). Nothing in reflash_block's ~190-line body ever reads it -- selecting "test write" in the legacy UI erases and
// reflashes the TCU exactly like "write", differing only in a log line. This plan/executor pair has no dry-run to port,
// so TestWrite is rejected outright with ErrorKind::kUnsupported rather than silently performing a live write.
//
// Deliberate divergence 2 (read region, same shape as
// subaru_tcu_cvt_hitachi_m32r_can): read_mem computes start_addr = start_addr -
// 0x00100000 with start_addr == fblocks[0].start == 0, which underflows
// uint32_t to 0xFFF00000 and bypasses the "< 0x8000" floor clamp entirely --
// 0xFFF00000 is not less than 0x8000, so the clamp's own intent (read starts no
// earlier than 0x8000) never took effect on real hardware. This plan targets
// that evident intent directly rather than reproducing the underflowed address
// nothing ever observed on the wire.
struct SubaruTcuHitachiM32rCanPlan
{
    std::uint32_t request_id;       // 0x7E1
    std::uint32_t response_id;      // 0x7E9
    int bitrate;                    // 500000
    bool extended_id;               // false
    std::uint32_t page_size;        // 0x100, the kernel's dump page size (read_mem)
    std::uint32_t write_frame_size; // 128, the kernel's 0xB6 write frame size (reflash_block)
};

// Requires no kernel: Connects to the TCU's resident on-board kernel
// (connect_bootloader) and drives it with page-read/block-write commands
// over CAN; no image is uploaded.
template <> struct FamilyTraits<SubaruTcuHitachiM32rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruTcuHitachiM32rCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruTcuHitachiM32rCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

Result<FlashPlan> BuildSubaruTcuHitachiM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<bytes::Bytes> image);

Status ValidateSubaruTcuHitachiM32rCanPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruTcuHitachiM32rCanProtocols();
} // namespace fastecu::flash
