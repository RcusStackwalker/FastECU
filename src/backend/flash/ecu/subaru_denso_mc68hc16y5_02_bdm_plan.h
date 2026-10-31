#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "src/backend/flash/flash_family.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{
// The BDM bridge speaks a fixed-baud ASCII command
// protocol; the baud is the only wire parameter.
struct SubaruDensoMc68hc16y5_02BdmPlan
{
    int baud;
};

// Requires no kernel: Write uploads the cfg kernel over BDM, but carries
// it as the plan image (the bytes written to RAM), not as a KernelImage.
template <> struct FamilyTraits<SubaruDensoMc68hc16y5_02BdmPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoMc68hc16y502Bdm;
    static constexpr TransportKind kTransport = TransportKind::kKline;
    static constexpr bool kRequiresKernel = false;
};

template <> struct PlanValidator<SubaruDensoMc68hc16y5_02BdmPlan>
{
    static Status Validate(const FlashPlan& plan);
};

// Legacy write_mem() pads the kernel to 32 bytes and flash_block() uploads it
// in 32-byte chunks. Defined once so the plan (padding) and the executor
// (chunking) cannot diverge.
inline constexpr std::size_t kSubaruDensoMc68hc16y5_02BdmUploadChunk = 0x20;

// Read returns the 0x0-0x2FFFF address-space image. Write is a kernel
// bootstrap: `kernel` is uploaded to RAM and started; the ROM is never
// written, so `rom_image` must be absent for every operation.
Result<FlashPlan> BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> rom_image,
                                                      std::optional<KernelImage> kernel);
Status ValidateSubaruDensoMc68hc16y502BdmPlan(const FlashPlan& plan);

// Exact protocol ids routed to this family, in table order.
std::span<const std::string_view> SubaruDensoMc68hc16y502BdmProtocols();
} // namespace fastecu::flash
