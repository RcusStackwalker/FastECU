#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

#include "src/backend/flash/ecu/denso_iso15765_bootloader_profile.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// Legacy: flash_ecu_subaru_denso_1n83m_1_5m_can_operation.{h,cpp}. Single
// protocol variant. The region fields carry what legacy hardcoded in
// read_memory and the 0x34/0x35 setup PDUs: kFlashBlocksN83M_1_5MB[1] exactly.
struct SubaruDenso1n83m_1_5mCanPlan
{
    std::uint32_t request_id;   // 0x7e0
    std::uint32_t response_id;  // 0x7e8
    int bitrate;                // 500000
    bool extended_id;           // false
    std::uint32_t lead_pad_len; // 0x10000, prepended to a read image as 0xFF
    std::uint32_t tail_pad_len; // 0x100, appended to a read image as 0xFF
};

// Requires no kernel: Jumps to the ECU's resident on-board kernel via
// 0x10 0x42 (bench) or 0x10 0x62 (in-car), uploading no image.
template <> struct FamilyTraits<SubaruDenso1n83m_1_5mCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDenso1n83m15mCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruDenso1n83m_1_5mCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

// The family's dialect profile.
template <> struct DensoIso15765BootloaderProfileOf<SubaruDenso1n83m_1_5mCanPlan>
{
    static constexpr DensoIso15765BootloaderProfile kValue{
        // Legacy names the 4MB sibling in the connect line and the two "using
        // CAN" lines -- an upstream copy-paste that the 4M sibling carries
        // legitimately. Corrected here rather than transcribed: this family's
        // own error strings already say 1.5M, so leaving these would make a
        // flash log unattributable to the family that produced it. A
        // deliberate, purely cosmetic divergence from the legacy text; do not
        // "restore" 4MB.
        .unit = "Subaru Denso 1N83M 1.5MB",
        .obk_active_log = "OBK is active", // OBK probe
        .identity_query = DensoIdentityQuery::kEcuIdService,
        // The tolerant probes in connect_bootloader read with
        // serial_read_short_timeout.
        .probe_timeout = std::chrono::milliseconds{200},
        .in_car_run = DensoInCarRun::kN83m,
        .in_car_timeout = std::chrono::milliseconds{200},
        // The bench arm reads once after the kernel jump: no discarded reply.
        .bench = {.settle = std::chrono::milliseconds{500},
                  .session_reply = DensoBenchSessionReply::kFatal,
                  .session_timeout = std::chrono::milliseconds{200},
                  .jump_first_read = std::chrono::milliseconds{200},
                  .jump_loop_read = std::chrono::milliseconds{200},
                  .discard_first_reply = false},
        // Both dump-setup exchanges are fatal on a mismatch and on an absent
        // reply, read at serial_read_short_timeout.
        .read_setup_reply = DensoReadSetupReply::kFatal,
        .read_setup_policy = {.read_timeout = std::chrono::milliseconds{200}},
        .pace_pages = false,
        .region_log = DensoRegionLog::kDumpAndBlock,
        // write_memory: block_modified is {0, 1, 0} over numblocks == 3, so
        // fblocks[1] -- the plan's transfer region -- is the one block erased
        // and reflashed.
        .write_block_index = 1,
        // Checksum verify reads twice with receive_timeout: the first read at
        // and, after the ECU's 7F 31 78 pending answer, the re-read.
        // UdsClient's 3000 ms pending default is not this family's number, so
        // the policy pins it to 500 as well.
        .checksum_policy = {.read_timeout = std::chrono::milliseconds{500},
                            .pending_timeout = std::chrono::milliseconds{500}},
        // kFlashBlocksN83M_1_5MB[0].start -- reflash_block indexes the image as
        // newdata[i + blockaddr - fdt->fblocks[0].start], so byte 0 of the plan
        // image is this address.
        .image_start = 0x08F9C000,
    };
};

Result<FlashPlan> BuildSubaruDenso1n83m15mCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                  std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruDenso1n83m15mCanPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDenso1n83m15mCanProtocols();
} // namespace fastecu::flash
