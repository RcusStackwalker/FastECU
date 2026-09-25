#pragma once

#include <cstddef>

namespace fastecu::flash
{
// Step 5 tail, wave 6c-1. The BDM bridge speaks a fixed-baud ASCII command
// protocol; the baud is the only wire parameter.
struct SubaruDensoMc68hc16y5_02BdmPlan
{
    int baud;
};

// Legacy write_mem() pads the kernel to 32 bytes (:247-250) and flash_block()
// uploads it in 32-byte chunks (:361). Defined once so the plan (padding) and
// the executor (chunking) cannot diverge.
inline constexpr std::size_t kSubaruDensoMc68hc16y5_02BdmUploadChunk = 0x20;
} // namespace fastecu::flash
