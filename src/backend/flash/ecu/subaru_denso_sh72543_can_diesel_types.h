#pragma once
#include <chrono>
#include <cstdint>

#include "src/backend/flash/ecu/denso_iso15765_bootloader_profile.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_denso_sh72543_can_diesel_operation.{h,cpp}. Region
// fields carry kFlashBlocksSH72543d[0] exactly -- what execute() passes to
// read_memory and what read_memory's 0x34/0x35 setup PDUs then compute from
// those arguments. Unlike its three siblings this family does not overwrite the
// arguments: that hardcode is commented out.
struct SubaruDensoSh72543CanDieselPlan
{
    std::uint32_t request_id;   // 0x7e0
    std::uint32_t response_id;  // 0x7e8
    int bitrate;                // 500000
    bool extended_id;           // false
    std::uint32_t lead_pad_len; // 0x8000, prepended to a read image as 0xFF
    std::uint32_t tail_pad_len; // 0x100, appended to a read image as 0xFF
};

template <> struct FamilyTraits<SubaruDensoSh72543CanDieselPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh72543CanDiesel;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
};

template <> struct PlanValidator<SubaruDensoSh72543CanDieselPlan>
{
    static Status Validate(const FlashPlan& plan);
};

// The family's dialect profile. Where its 1N83M 1.5M and SH72531 siblings read with the 200 ms
// serial_read_short_timeout, this family mostly reads with the 2000 ms
// serial_read_timeout.
template <> struct DensoIso15765BootloaderProfileOf<SubaruDensoSh72543CanDieselPlan>
{
    static constexpr DensoIso15765BootloaderProfile kValue{
        .unit = "Subaru Denso SH72543 Diesel",
        // OBK probe: the only connect exchange that reads with
        // serial_read_short_timeout.
        .obk_active_log = "OBK is active!",
        // Unlike its three siblings, which query the ECU id with the vendor SID
        // 0xAA, this family reads it with a standard ReadDataByIdentifier of
        // DID F182 and checks the whole echoed `62 F1 82` header.
        .identity_query = DensoIdentityQuery::kDidF182,
        // THIS FAMILY DIFFERS: its tolerant probes read with
        // serial_read_timeout where its three siblings use
        // serial_read_short_timeout.
        .probe_timeout = std::chrono::milliseconds{2000},
        .in_car_run = DensoInCarRun::kSh,
        .in_car_timeout = std::chrono::milliseconds{2000},
        // Bench programming arm: it waits 50 ms where the in-car arm waits 500
        // and holds the family's one commented-out `return STATUS_ERROR`: a wrong
        // `50 43` answer is logged and stepped over, and only an absent or
        // too-short reply aborts. Its three siblings abort there. The jump
        // reads first at serial_read_timeout, then at
        // serial_read_short_timeout.
        .bench = {.settle = std::chrono::milliseconds{50},
                  .session_reply = DensoBenchSessionReply::kTolerantProbe,
                  .session_timeout = std::chrono::milliseconds{2000},
                  .jump_first_read = std::chrono::milliseconds{2000},
                  .jump_loop_read = std::chrono::milliseconds{200},
                  .discard_first_reply = false},
        // Both setup exchanges are fatal on a mismatch and on an absent reply,
        // and both read with serial_read_short_timeout -- the two reads in this
        // family that do not use the long timeout.
        .read_setup_reply = DensoReadSetupReply::kFatal,
        .read_setup_policy = {.read_timeout = std::chrono::milliseconds{200}},
        .pace_pages = true,
        // Legacy's own log line for all three "start & length" announcements.
        .region_log = DensoRegionLog::kStartAndLength,
        // write_memory: block_modified is {1} over numblocks == 1, so block 0
        // -- kFlashBlocksSH72543d[0] -- is the one block reflashed. The table's
        // 0x0-0x8000 entry is commented out.
        .write_block_index = 0,
        // Checksum verify reads twice with serial_read_timeout: and, after the
        // ECU's 7F 31 78 pending answer, the re-read.
        .checksum_policy = {.read_timeout = std::chrono::milliseconds{2000},
                            .pending_timeout = std::chrono::milliseconds{2000}},
        // Legacy write_memory hands reflash_block
        // &data_array[fdt->fblocks->start], the encrypted whole-ROM buffer offset
        // by fblocks[0].start = 0x8000, and reflash_block then indexes newdata[i +
        // blockctr * blocksize] -- together data_array[blockaddr + i]: the
        // absolute address, matching read_memory's own image base and the three
        // sibling families. The original port design's "write-base divergence"
        // reads alone and concludes legacy wrote a full ROM 0x8000 low; with the
        // caller's pre-offset it does not. Do not "restore" a 0x8000 shift on its
        // strength.
        .image_start = 0x00000000,
    };
};

} // namespace fastecu::flash
