#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_types.h"
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
    SubaruDensoSh7058CanSecurity security;
};

// Exact identities from the revision-59f4e442 legacy wrapper/configuration.
// Security selection replaces the legacy suffix chain; the executor consumes
// only this enum and never reinterprets target_id().
constexpr auto kCatalog = std::to_array<CatalogEntry>({
    {"sub_ecu_denso_sh7058_can", SubaruDensoSh7058CanSecurity::kStock},
    {"sub_ecu_denso_sh7058_can_ecutek", SubaruDensoSh7058CanSecurity::kEcuTek},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", SubaruDensoSh7058CanSecurity::kRaceRom},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", SubaruDensoSh7058CanSecurity::kRaceRomAlt},
    {"sub_ecu_denso_sh7058_can_cobb", SubaruDensoSh7058CanSecurity::kCobb},
});

constexpr std::string_view kMcu = "SH7058";
constexpr std::uint32_t kRomSize = 0x00100000;
constexpr std::uint32_t kKernelLoadAddress = 0xFFFF3000;

Result<CatalogEntry> GetCatalogEntry(std::string_view protocol, std::string_view mcu)
{
    const auto it = std::ranges::find(kCatalog, protocol, &CatalogEntry::protocol);
    if (it == kCatalog.end())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Unsupported Subaru Denso SH7058 petrol CAN protocol: {}", protocol));
    }
    if (mcu != kMcu)
    {
        return Fail(
            ErrorKind::kInvalidConfig,
            std::format("Subaru Denso SH7058 petrol CAN protocol {} requires MCU SH7058, not {}", protocol, mcu));
    }
    return *it;
}

const FlashDevice *CheckedDevice()
{
    const FlashDevice *device = FindFlashDevice(kMcu);
    if (!detail::FixedShapeDeviceMatches(device, kRomSize, 16))
    {
        return nullptr;
    }
    return device;
}

bool WireParametersMatch(const SubaruDensoSh7058CanPlan& wire, const CatalogEntry& entry)
{
    return wire.request_id == 0x7E0 && wire.response_id == 0x7E8 && wire.bitrate == 500000 && !wire.extended_id &&
           wire.security == entry.security;
}

} // namespace

Status ValidateSubaruDensoSh7058CanPlan(const FlashPlan& plan)
{
    const auto entry = GetCatalogEntry(plan.TargetId(), plan.McuName());
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    const auto *wire = plan.FamilyPlan().As<SubaruDensoSh7058CanPlan>();
    if (wire == nullptr || !WireParametersMatch(*wire, *entry))
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 CAN wire/security parameters are invalid");
    }
    const FlashDevice *device = CheckedDevice();
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 catalog does not match the flash device table");
    }
    if (!plan.Confirmations().empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 plans must not declare extra confirmations");
    }
    return detail::ValidateWholeRomKernelPlan<128>(plan, kKernelLoadAddress, *device);
}

Result<FlashPlan> BuildSubaruDensoSh7058CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                KernelImage kernel)
{
    const auto entry = GetCatalogEntry(protocol_name, mcu_type);
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    const FlashDevice *device = CheckedDevice();
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 catalog does not match the flash device table");
    }
    return detail::BuildWholeRomKernelPlan<128>(
        {
            .operation = operation,
            .target_id = std::string(protocol_name),
            .mcu_name = std::string(mcu_type),
            .family_plan = SubaruDensoSh7058CanPlan{.security = entry->security},
        },
        std::move(image), std::move(kernel), kKernelLoadAddress, *device, ValidateSubaruDensoSh7058CanPlan);
}

constexpr auto kExportedProtocols = detail::ProtocolIdsOf<&CatalogEntry::protocol>(kCatalog);

std::span<const std::string_view> SubaruDensoSh7058CanProtocols()
{
    return kExportedProtocols;
}

Status PlanValidator<SubaruDensoSh7058CanPlan>::Validate(const FlashPlan& plan)
{
    return ValidateSubaruDensoSh7058CanPlan(plan);
}

} // namespace fastecu::flash
