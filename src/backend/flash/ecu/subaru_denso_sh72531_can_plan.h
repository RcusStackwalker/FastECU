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
// Legacy: flash_ecu_subaru_denso_sh72531_can_operation.{h,cpp}. Region fields
// carry what legacy hardcoded in read_memory and the 0x34/0x35 setup PDUs:
// kFlashBlocksSH72531[1] exactly.
struct SubaruDensoSh72531CanPlan
{
    std::uint32_t request_id;   // 0x7e0
    std::uint32_t response_id;  // 0x7e8
    int bitrate;                // 500000
    bool extended_id;           // false
    std::uint32_t lead_pad_len; // 0x8000, prepended to a read image as 0xFF
    std::uint32_t tail_pad_len; // 0x100, appended to a read image as 0xFF
};

// Requires no kernel: Same resident on-board kernel jump as its 1N83M
// sibling, via 0x10 0x42 (bench) or 0x10 0x62 (in-car); no image is uploaded.
template <> struct FamilyTraits<SubaruDensoSh72531CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh72531Can;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruDensoSh72531CanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

// The family's dialect profile. Legacy reaches most of its timeouts through
// bare literals, so the values, not the names, are authoritative.
template <> struct DensoIso15765BootloaderProfileOf<SubaruDensoSh72531CanPlan>
{
    static constexpr DensoIso15765BootloaderProfile kValue{
        .unit = "Subaru Denso SH72531",
        .obk_active_log = "OBK is active!", // OBK probe
        .identity_query = DensoIdentityQuery::kEcuIdService,
        // The tolerant probes in connect_bootloader read with
        // serial_read_short_timeout, as in two of its three siblings.
        .probe_timeout = std::chrono::milliseconds{200},
        .in_car_run = DensoInCarRun::kSh,
        .in_car_timeout = std::chrono::milliseconds{200},
        .bench = {.settle = std::chrono::milliseconds{500},
                  .session_reply = DensoBenchSessionReply::kFatal,
                  .session_timeout = std::chrono::milliseconds{200},
                  .jump_first_read = std::chrono::milliseconds{200},
                  .jump_loop_read = std::chrono::milliseconds{200},
                  .discard_first_reply = true},
        // Both setup exchanges are fatal on a mismatch and on an absent reply,
        // and both read with serial_read_timeout, not the short timeout its
        // 1N83M 1.5M sibling uses here.
        .read_setup_reply = DensoReadSetupReply::kFatal,
        .read_setup_policy = {.read_timeout = std::chrono::milliseconds{2000}},
        .pace_pages = true,
        .region_log = DensoRegionLog::kDumpAndBlock,
        // write_memory: block_modified is {0, 1, 0} over numblocks == 3, so
        // fblocks[1] is the one block reflashed.
        .write_block_index = 1,
        // Checksum verify reads twice with serial_read_timeout: and, after the
        // ECU's 7F 31 78 pending answer, the re-read. UdsClient's 3000 ms
        // pending default is not this family's number.
        .checksum_policy = {.read_timeout = std::chrono::milliseconds{2000},
                            .pending_timeout = std::chrono::milliseconds{2000}},
        // kFlashBlocksSH72531[0].start. write_memory hands reflash_block
        // &data_array[fdt->fblocks->start], i.e. fblocks[0].start, and
        // reflash_block then indexes newdata[i + blockaddr] -- so byte 0 of the
        // plan image is this address, which for SH72531 is 0.
        .image_start = 0x00000000,
    };
};

Result<FlashPlan> BuildSubaruDensoSh72531CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                 std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status ValidateSubaruDensoSh72531CanPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoSh72531CanProtocols();
} // namespace fastecu::flash
