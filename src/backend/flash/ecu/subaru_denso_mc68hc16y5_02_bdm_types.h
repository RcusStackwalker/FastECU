#pragma once

#include <cstddef>

#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// The BDM bridge speaks a fixed-baud ASCII command
// protocol; the baud is the only wire parameter.
struct SubaruDensoMc68hc16y5_02BdmPlan
{
    int baud;
};

template <> struct FamilyTraits<SubaruDensoMc68hc16y5_02BdmPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoMc68hc16y502Bdm;
    static constexpr TransportKind kTransport = TransportKind::kKline;
};

template <> struct PlanValidator<SubaruDensoMc68hc16y5_02BdmPlan>
{
    static Status Validate(const FlashPlan& plan);
};

// Legacy write_mem() pads the kernel to 32 bytes and flash_block() uploads it
// in 32-byte chunks. Defined once so the plan (padding) and the executor
// (chunking) cannot diverge.
inline constexpr std::size_t kSubaruDensoMc68hc16y5_02BdmUploadChunk = 0x20;
} // namespace fastecu::flash
