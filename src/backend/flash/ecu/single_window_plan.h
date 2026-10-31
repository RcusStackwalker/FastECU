#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

// A "single-window" family is one whose plan is fully described by a protocol
// id (or a small set of them), one MCU, a read window, a write window, and one
// image size, and which declares no confirmations. Families that carry kernel
// images, variant tables or confirmations keep their own hand-written
// builders.
//
// This shares plan *validation* only. Four of these families -- the Denso
// ISO-15765 bootloader dialect -- share one executor and state their
// differences as dialect profiles; the other executors stay un-factored,
// because their look-alike blocks differ in timeouts, retry counts and
// response strictness.
struct SingleWindowPlanSpec
{
    // The one qualified name every message composes from, e.g.
    // "Subaru Denso SH72531 CAN".
    std::string_view display_name;
    // One entry for most families; two for subaru_hitachi_m32r_kline and
    // subaru_hitachi_sh72543r_can, which accept a normal and a recovery
    // protocol name.
    std::span<const std::string_view> protocols;
    std::string_view mcu;
    MemoryRegion read_region;
    // Also the erase region: no family in this cluster erases anything other
    // than the window it writes.
    MemoryRegion write_region;
    std::uint32_t image_size;
    // False when the shared kFlashDevices[] entry is not what this family
    // expects. Block counts, and which blocks matter, differ per family, so
    // the check stays with the family; the core composes the message.
    bool (*geometry_ok)(const FlashDevice& device);
    // False when plan.family_plan() holds the wrong alternative or carries
    // wrong wire parameters. Takes the whole plan because
    // subaru_hitachi_m32r_kline's expected session mode depends on which
    // protocol name was used.
    bool (*wire_params_ok)(const FlashPlan& plan);
    // False for families whose legacy class never implemented a write path.
    // Defaults true, so the ten families that predate this flag are
    // unchanged.
    bool supports_write = true;
};

Status ValidateSingleWindowPlan(const SingleWindowPlanSpec& spec, const FlashPlan& plan);

Result<FlashPlan> BuildSingleWindowPlan(const SingleWindowPlanSpec& spec, FlashOperation operation,
                                        std::string_view protocol_name, std::string_view mcu_type,
                                        std::optional<bytes::Bytes> image, FamilyPlan family_plan);

} // namespace fastecu::flash
