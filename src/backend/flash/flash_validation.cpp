#include "src/backend/flash/flash_validation.h"

#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fastecu::flash
{
namespace
{

// True when the region's end address (start + length, one past its last byte)
// cannot be held in a uint32_t.
//
// This is deliberately one stricter than "the region fits in the address
// space": a region whose last byte is 0xffffffff ends at 0x100000000 and is
// rejected here even though every byte of it is addressable. Executors
// compute that end address in uint32_t arithmetic and bound real erase and
// write loops with it -- ecu/mitsu_colt_m32r_can_executor.cpp derives
// writable_end and page_write_end that way, and ecu/mitsu_colt_m32r_can_plan.cpp
// sizes the image against it -- where 0x100000000 would wrap to 0 and collapse
// the window. Accepting it here would relax an address-window guard. The
// boundary is pinned by TransferRegionEndingExactlyAtTheTopOfTheAddressSpaceIsRejected.
bool region_end_not_representable(const MemoryRegion& region)
{
    return static_cast<std::uint64_t>(region.start) + region.length > static_cast<std::uint64_t>(0xffffffffU);
}

bool family_matches_transport_variant(const FlashPlanFields& fields)
{
    switch (fields.family)
    {
    case FlashFamily::DensoSh705xEepromKline:
        return fields.transport == TransportKind::Kline &&
               std::holds_alternative<DensoSh705xEepromKlinePlan>(fields.family_plan);
    case FlashFamily::DensoSh705xEepromCan:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<DensoSh705xEepromCanPlan>(fields.family_plan);
    case FlashFamily::MitsuColtM32rCan:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<MitsuColtM32rCanPlan>(fields.family_plan);
    case FlashFamily::SubaruMitsuM32rKline:
        return fields.transport == TransportKind::Kline &&
               std::holds_alternative<SubaruMitsuM32rKlinePlan>(fields.family_plan);
    case FlashFamily::SubaruHitachiM32rKline:
        return fields.transport == TransportKind::Kline &&
               std::holds_alternative<SubaruHitachiM32rKlinePlan>(fields.family_plan);
    case FlashFamily::SubaruDensoMc68hc16y5_02:
        return fields.transport == TransportKind::Kline &&
               std::holds_alternative<SubaruDensoMc68hc16y5_02Plan>(fields.family_plan);
    case FlashFamily::SubaruDensoSh7055_02:
        return fields.transport == TransportKind::Kline &&
               std::holds_alternative<SubaruDensoSh7055_02Plan>(fields.family_plan);
    case FlashFamily::SubaruHitachiM32rCan:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<SubaruHitachiM32rCanPlan>(fields.family_plan);
    case FlashFamily::SubaruTcuCvtHitachiM32rCan:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<SubaruTcuCvtHitachiM32rCanPlan>(fields.family_plan);
    case FlashFamily::SubaruTcuCvtMitsuMh8111Can:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<SubaruTcuCvtMitsuMh8111CanPlan>(fields.family_plan);
    case FlashFamily::SubaruTcuCvtMitsuMh8104Can:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<SubaruTcuCvtMitsuMh8104CanPlan>(fields.family_plan);
    case FlashFamily::SubaruDenso1n83m_1_5mCan:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<SubaruDenso1n83m_1_5mCanPlan>(fields.family_plan);
    case FlashFamily::SubaruDensoSh72531Can:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<SubaruDensoSh72531CanPlan>(fields.family_plan);
    case FlashFamily::SubaruDensoSh72543CanDiesel:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<SubaruDensoSh72543CanDieselPlan>(fields.family_plan);
    case FlashFamily::SubaruDenso1n83m_4mCan:
        return fields.transport == TransportKind::CanIso15765 &&
               std::holds_alternative<SubaruDenso1n83m_4mCanPlan>(fields.family_plan);
    }
    return false;
}

} // namespace

Result<FlashPlan> validate_and_build(FlashPlanFields fields)
{
    if (fields.target_id.empty())
    {
        return fail(ErrorKind::InvalidConfig, "target_id must not be empty");
    }
    if (fields.mcu_name.empty())
    {
        return fail(ErrorKind::InvalidConfig, "mcu_name must not be empty");
    }
    if (fields.transfer_region.length == 0)
    {
        return fail(ErrorKind::InvalidConfig, "transfer_region must not be empty");
    }
    if (region_end_not_representable(fields.transfer_region))
    {
        return fail(ErrorKind::InvalidConfig, "transfer_region end address does not fit in 32 bits");
    }
    for (const MemoryRegion& erase : fields.erase_regions)
    {
        if (region_end_not_representable(erase))
        {
            return fail(ErrorKind::InvalidConfig, "erase region end address does not fit in 32 bits");
        }
    }
    if (fields.operation == FlashOperation::Read)
    {
        if (!fields.erase_regions.empty())
        {
            return fail(ErrorKind::InvalidConfig, "Read plans must not declare erase regions");
        }
        if (fields.image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "Read plans must not carry an image");
        }
    }
    else
    {
        if (!fields.image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "Write/TestWrite plans must carry an image");
        }
    }
    if (const bool requires_kernel =
            std::visit([]<typename T>(const T&) { return family_requires_kernel_v<T>; }, fields.family_plan);
        requires_kernel && !fields.kernel.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "family requires a kernel image");
    }
    if (fields.kernel.has_value())
    {
        if (fields.kernel->id.empty())
        {
            return fail(ErrorKind::InvalidConfig, "kernel id must not be empty");
        }
        if (fields.kernel->bytes.empty())
        {
            return fail(ErrorKind::InvalidConfig, "kernel bytes must not be empty");
        }
        const std::uint64_t kernel_end =
            static_cast<std::uint64_t>(fields.kernel->load_address) + fields.kernel->bytes.size();
        if (kernel_end > static_cast<std::uint64_t>(0xffffffffU))
        {
            return fail(ErrorKind::InvalidConfig, "kernel upload range overflows a 32-bit address space");
        }
    }
    if (!family_matches_transport_variant(fields))
    {
        return fail(ErrorKind::InvalidConfig, "family_plan variant does not match transport kind or declared family");
    }
    std::unordered_set<ConfirmationSpec::Id> seen_ids;
    for (const ConfirmationSpec& confirmation : fields.confirmations)
    {
        if (!seen_ids.insert(confirmation.id).second)
        {
            return fail(ErrorKind::InvalidConfig, "duplicate confirmation id declared");
        }
    }

    const std::uint64_t total_transfer_bytes = fields.transfer_region.length;
    return FlashPlan(std::move(fields), total_transfer_bytes);
}

} // namespace fastecu::flash
