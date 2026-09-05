#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{

// A "single-window" family is one whose plan is fully described by a protocol
// id (or a small set of them), one MCU, a read window, a write window, and one
// image size. Ten families in this package share that shape. The three that do
// not -- mc68hc16y5_02, sh7055_02 and mitsu_colt, which carry kernel images,
// variant tables or confirmations -- keep their own hand-written builders.
//
// This shares plan *validation* only. The executors of these same families
// stay deliberately un-factored, because their look-alike blocks differ in
// timeouts, retry counts and response strictness: see
// denso_iso15765_can_common.h and docs/protocol-generalization-opportunities.md.
struct SingleWindowPlanSpec
{
    // The one qualified name every message composes from, e.g.
    // "Subaru Denso SH72531 CAN".
    std::string_view display_name;
    // One entry for most families; two for subaru_hitachi_m32r_kline, which
    // accepts a normal and a recovery protocol name.
    std::span<const std::string_view> protocols;
    std::string_view mcu;
    FlashFamily family;
    TransportKind transport;
    MemoryRegion read_region;
    // Also the erase region: no family in this cluster erases anything other
    // than the window it writes.
    MemoryRegion write_region;
    std::uint32_t image_size;
    // False when the shared flashdevices[] entry is not what this family
    // expects. Block counts, and which blocks matter, differ per family, so
    // the check stays with the family; the core composes the message.
    bool (*geometry_ok)(const flashdev_t& device);
    // False when plan.family_plan() holds the wrong alternative or carries
    // wrong wire parameters. Takes the whole plan because
    // subaru_hitachi_m32r_kline's expected session mode depends on which
    // protocol name was used.
    bool (*wire_params_ok)(const FlashPlan& plan);
};

Status validate_single_window_plan(const SingleWindowPlanSpec& spec, const FlashPlan& plan);

Result<FlashPlan> build_single_window_plan(const SingleWindowPlanSpec& spec, FlashOperation operation,
                                           std::string_view protocol_name, std::string_view mcu_type,
                                           std::optional<bytes::Bytes> image, FamilyPlan family_plan);

} // namespace fastecu::flash
