#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"

#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/ecu/plan_primitives.h"

namespace fastecu::flash
{
namespace
{

struct CatalogEntry
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
    std::uint32_t kernel_load_address;
};

// Exact identities from the built-in catalog (src/backend/config/builtin_catalog.cpp) and the diesel
// legacy operation's EURO4/EURO5 comments. The executor consumes the selected plan and never derives
// a generation from a target-id suffix.
constexpr auto kCatalog = std::to_array<CatalogEntry>({
    {"sub_ecu_denso_sh7058_can_diesel", "SH7058d", 0x00100000, 0xFFFF4000},
    {"sub_ecu_denso_sh7059_can_diesel", "SH7059d", 0x00180000, 0xFFFEE000},
});

const FlashDevice *CheckedDevice(const CatalogEntry& entry)
{
    const FlashDevice *device = FindFlashDevice(entry.mcu);
    if (!detail::FixedShapeDeviceMatches(device, entry.rom_size, 16))
    {
        return nullptr;
    }
    return device;
}

bool WireParametersMatch(const SubaruDensoSh7058CanDieselPlan& wire)
{
    return wire.request_id == 0x7E0 && wire.response_id == 0x7E8 && wire.bitrate == 500000 && !wire.extended_id;
}

} // namespace

Status ValidateSubaruDensoSh7058CanDieselPlan(const FlashPlan& plan)
{
    const auto entry =
        detail::FindCatalogEntry(kCatalog, "Subaru Denso SH705x diesel CAN", plan.TargetId(), plan.McuName());
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    const auto *wire = std::get_if<SubaruDensoSh7058CanDieselPlan>(&plan.FamilyPlan());
    if (wire == nullptr || !WireParametersMatch(*wire))
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel CAN wire parameters are invalid");
    }
    const FlashDevice *device = CheckedDevice(*entry);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel catalog does not match the flash device table");
    }
    if (!plan.Confirmations().empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel plans must not declare extra confirmations");
    }
    return detail::ValidateWholeRomKernelPlan<128>(plan, entry->kernel_load_address, *device);
}

Result<FlashPlan> BuildSubaruDensoSh7058CanDieselPlan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                      KernelImage kernel)
{
    const auto entry = detail::FindCatalogEntry(kCatalog, "Subaru Denso SH705x diesel CAN", protocol_name, mcu_type);
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    const FlashDevice *device = CheckedDevice(*entry);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel catalog does not match the flash device table");
    }
    return detail::BuildWholeRomKernelPlan<128>(
        {
            .operation = operation,
            .family = FlashFamily::kSubaruDensoSh7058CanDiesel,
            .transport = TransportKind::kCanIso15765,
            .target_id = std::string(protocol_name),
            .mcu_name = std::string(mcu_type),
            .family_plan = SubaruDensoSh7058CanDieselPlan{},
        },
        std::move(image), std::move(kernel), entry->kernel_load_address, *device,
        ValidateSubaruDensoSh7058CanDieselPlan);
}

constexpr auto kExportedProtocols = detail::ProtocolIdsOf<&CatalogEntry::protocol>(kCatalog);

std::span<const std::string_view> SubaruDensoSh7058CanDieselProtocols()
{
    return kExportedProtocols;
}

Status PlanValidator<SubaruDensoSh7058CanDieselPlan>::Validate(const FlashPlan& plan)
{
    return ValidateSubaruDensoSh7058CanDieselPlan(plan);
}

} // namespace fastecu::flash
