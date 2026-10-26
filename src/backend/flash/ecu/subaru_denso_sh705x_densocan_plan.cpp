#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <utility>

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_types.h"
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

// Exact built-in catalog/legacy identities. These are intentionally exact
// identities rather than a suffix rule: EEPROM DensoCAN and future variants
// are a different family until designed and tested explicitly.
constexpr auto kCatalog =
    std::to_array<CatalogEntry>({{"sub_ecu_denso_sh7055_densocan", "SH7055", 0x00080000, 0xFFFF6004},
                                 {"sub_ecu_denso_sh7058_densocan", "SH7058", 0x00100000, 0xFFFF3000},
                                 {"sub_ecu_denso_sh7058s_densocan", "SH7058", 0x00100000, 0xFFFF3000},
                                 {"sub_ecu_denso_sh7058s_diesel_densocan", "SH7058", 0x00100000, 0xFFFF3000},
                                 {"sub_ecu_denso_sh7059_diesel_densocan", "SH7059d", 0x00180000, 0xFFFEE000}});

Result<CatalogEntry> GetCatalogEntry(std::string_view protocol, std::string_view mcu)
{
    const auto it = std::ranges::find(kCatalog, protocol, &CatalogEntry::protocol);
    if (it == kCatalog.end())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("Unsupported DensoCAN protocol: {}", protocol));
    }
    if (mcu != it->mcu)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("DensoCAN protocol {} requires MCU {}, not {}", protocol, it->mcu, mcu));
    }
    return *it;
}

const FlashDevice *CheckedDevice(const CatalogEntry& entry)
{
    const FlashDevice *device = FindFlashDevice(entry.mcu);
    if (!detail::FixedShapeDeviceMatches(device, entry.rom_size, 16))
    {
        return nullptr;
    }
    return device;
}

SubaruDensoSh705xDensoCanPlan WireParameters()
{
    return {};
}

bool WireParametersMatch(const SubaruDensoSh705xDensoCanPlan& wire)
{
    return wire.iso_request_id == 0x7E0 && wire.iso_response_id == 0x7E8 && wire.raw_transmit_id == 0x000FFFFE &&
           wire.raw_receive_id == 0x21 && wire.bitrate == 500000 && !wire.iso_extended_id && wire.raw_extended_id;
}

} // namespace

Status ValidateSubaruDensoSh705xDensocanPlan(const FlashPlan& plan)
{
    const auto *wire = std::get_if<SubaruDensoSh705xDensoCanPlan>(&plan.FamilyPlan());
    if (wire == nullptr || !WireParametersMatch(*wire))
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN wire parameters are invalid");
    }
    const auto entry = GetCatalogEntry(plan.TargetId(), plan.McuName());
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    const FlashDevice *device = CheckedDevice(*entry);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN catalog does not match the flash device table");
    }
    if (!detail::RequiresOnly(plan.Confirmations(), ConfirmationSpec::Id::kCycleIgnition))
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN requires exactly the CycleIgnition confirmation");
    }
    return detail::ValidateWholeRomKernelPlan<6>(plan, entry->kernel_load_address, *device);
}

Result<FlashPlan> BuildSubaruDensoSh705xDensocanPlan(FlashOperation operation, std::string_view protocol_name,
                                                     std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                     KernelImage kernel)
{
    const auto entry = GetCatalogEntry(protocol_name, mcu_type);
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    const FlashDevice *device = CheckedDevice(*entry);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN catalog does not match the flash device table");
    }
    return detail::BuildWholeRomKernelPlan<6>(
        {
            .operation = operation,
            .family = FlashFamily::kSubaruDensoSh705xDensoCan,
            .transport = TransportKind::kCanRawIso15765,
            .target_id = std::string(protocol_name),
            .mcu_name = std::string(mcu_type),
            .family_plan = WireParameters(),
            .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}},
        },
        std::move(image), std::move(kernel), entry->kernel_load_address, *device,
        ValidateSubaruDensoSh705xDensocanPlan);
}

} // namespace fastecu::flash
