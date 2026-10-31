#pragma once
#include <chrono>
#include <cstdint>

#include "src/backend/flash/ecu/denso_iso15765_bootloader_profile.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_denso_1n83m_4m_can_operation.{h,cpp}. Single
// protocol variant. The region fields carry what legacy hardcoded in
// read_memory and the 0x34/0x35 setup PDUs: kFlashBlocksN83M_4MB[1] exactly.
struct SubaruDenso1n83m_4mCanPlan
{
    std::uint32_t request_id;   // 0x7e0
    std::uint32_t response_id;  // 0x7e8
    int bitrate;                // 500000
    bool extended_id;           // false
    std::uint32_t lead_pad_len; // 0x10000, prepended to a read image as 0xFF
    std::uint32_t tail_pad_len; // 0x100, appended to a read image as 0xFF
};

// Requires no kernel: The 4MB variant of the 1N83M family: same resident
// on-board kernel jump via 0x10 0x42 (bench) or 0x10 0x62 (in-car), no image
// uploaded.
template <> struct FamilyTraits<SubaruDenso1n83m_4mCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDenso1n83m4mCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
};

template <> struct PlanValidator<SubaruDenso1n83m_4mCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

// The family's dialect profile, the tolerant member of the dialect. This family writes most timeouts as bare 200/500
// literals where the header declares serial_read_short_timeout = 200 and receive_timeout = 500; the numbers, not the
// spellings, are the wire behaviour.
template <> struct DensoIso15765BootloaderProfileOf<SubaruDenso1n83m_4mCanPlan>
{
    static constexpr DensoIso15765BootloaderProfile kValue{
        .unit = "Subaru Denso 1N83M 4MB",
        .obk_active_log = "OBK is active", // OBK probe
        .identity_query = DensoIdentityQuery::kEcuIdService,
        // The connect_bootloader probe sites have their `return STATUS_ERROR`
        // commented out, but the siblings log and carry on there too: no
        // divergence, serial_read_short_timeout as in two of its three
        // siblings.
        .probe_timeout = std::chrono::milliseconds{200},
        .in_car_run = DensoInCarRun::kN83m,
        .in_car_timeout = std::chrono::milliseconds{200},
        // It reads twice after the bench kernel jump, as SH72531 does.
        .bench = {.settle = std::chrono::milliseconds{500},
                  .session_reply = DensoBenchSessionReply::kFatal,
                  .session_timeout = std::chrono::milliseconds{200},
                  .jump_first_read = std::chrono::milliseconds{200},
                  .jump_loop_read = std::chrono::milliseconds{200},
                  .discard_first_reply = true},
        // TOLERATED, (the 0x34 RequestDownload setup) and 917 / 924 (the 0x35
        // RequestUpload setup): all four `return STATUS_ERROR` statements are
        // commented out, so neither a wrong reply nor an absent one stops the
        // dump. This is the family's one genuine behavioural divergence. Erase
        // setup stays fatal.
        .read_setup_reply = DensoReadSetupReply::kTolerantReply,
        .read_setup_policy = {.read_timeout = std::chrono::milliseconds{200}},
        .pace_pages = false,
        .region_log = DensoRegionLog::kDumpAndBlock,
        // write_memory: block_modified is {0, 1, 0} over numblocks == 3, so
        // fblocks[1] is the one block reflashed.
        .write_block_index = 1,
        // Checksum verify's two reads differ: the bare 500 literal
        // (receive_timeout) and, after the ECU's 7F 31 78 pending answer,
        // serial_read_extra_long_timeout.
        .checksum_policy = {.read_timeout = std::chrono::milliseconds{500},
                            .pending_timeout = std::chrono::milliseconds{3000}},
        // kFlashBlocksN83M_4MB[0].start -- reflash_block indexes the image as
        // newdata[i + blockaddr - fdt->fblocks[0].start] and write_memory hands
        // it &data_array[0], the whole encrypted ROM, so byte 0 of the plan
        // image is this address.
        .image_start = 0x08F9C000,
    };
};

} // namespace fastecu::flash
