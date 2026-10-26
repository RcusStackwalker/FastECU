#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_types.h"
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
    bool supports_write;
};

// Exact TCU identities and kernel addresses from the built-in catalog and
// FlashTcuSubaruDensoSH705xCanOperation at revision 59f4e442 (lines 55-58).
// Do not broaden this into a protocol suffix match.
constexpr auto kCatalog = std::to_array<CatalogEntry>({
    {"sub_tcu_denso_sh7055_can", "SH7055", 0x00080000, 0xFFFF9000, false},
    {"sub_tcu_denso_sh7058_can", "SH7058", 0x00100000, 0xFFFF3000, true},
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

SubaruTcuDensoSh705xCanPlan WireParameters()
{
    return {};
}

bool WireParametersMatch(const SubaruTcuDensoSh705xCanPlan& wire)
{
    return wire.request_id == 0x7E1 && wire.response_id == 0x7E9 && wire.bitrate == 500000 && !wire.extended_id;
}

Status ValidateCapability(const FlashPlan& plan, const CatalogEntry& entry)
{
    if (plan.Operation() == FlashOperation::kTestWrite ||
        (plan.Operation() == FlashOperation::kWrite && !entry.supports_write))
    {
        return Fail(ErrorKind::kUnsupported, "operation is not supported by the selected Denso SH705x TCU");
    }
    return {};
}

} // namespace

Status ValidateSubaruTcuDensoSh705xCanPlan(const FlashPlan& plan)
{
    const auto *wire = std::get_if<SubaruTcuDensoSh705xCanPlan>(&plan.FamilyPlan());
    if (wire == nullptr || !WireParametersMatch(*wire))
    {
        return Fail(ErrorKind::kInvalidConfig, "TCU CAN wire parameters are invalid");
    }
    const auto entry = detail::FindCatalogEntry(kCatalog, "Denso SH705x TCU CAN", plan.TargetId(), plan.McuName());
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    if (Status capability = ValidateCapability(plan, *entry); !capability.has_value())
    {
        return capability;
    }
    const FlashDevice *device = CheckedDevice(*entry);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "TCU catalog does not match the flash device table");
    }
    if (!plan.Confirmations().empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "TCU plans must not declare extra confirmations");
    }
    return detail::ValidateWholeRomKernelPlan<128>(plan, entry->kernel_load_address, *device);
}

Result<FlashPlan> BuildSubaruTcuDensoSh705xCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                   KernelImage kernel)
{
    const auto entry = detail::FindCatalogEntry(kCatalog, "Denso SH705x TCU CAN", protocol_name, mcu_type);
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    // Capability is checked before image/kernel validation, preserving the
    // SH7055 write and all test-write rejections before any hardware path.
    if (operation == FlashOperation::kTestWrite || (operation == FlashOperation::kWrite && !entry->supports_write))
    {
        return Fail(ErrorKind::kUnsupported, "operation is not supported by the selected Denso SH705x TCU");
    }
    const FlashDevice *device = CheckedDevice(*entry);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "TCU catalog does not match the flash device table");
    }
    return detail::BuildWholeRomKernelPlan<128>(
        {
            .operation = operation,
            .family = FlashFamily::kSubaruTcuDensoSh705xCan,
            .transport = TransportKind::kCanIso15765,
            .target_id = std::string(protocol_name),
            .mcu_name = std::string(mcu_type),
            .family_plan = WireParameters(),
        },
        std::move(image), std::move(kernel), entry->kernel_load_address, *device, ValidateSubaruTcuDensoSh705xCanPlan);
}

} // namespace fastecu::flash
