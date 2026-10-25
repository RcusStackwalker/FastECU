#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"

#include <array>
#include <cstddef>
#include <format>
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
// legacy operation's EURO4/EURO5 comments at revision 59f4e442:113-116. The
// executor consumes the selected plan and never derives a generation from a
// target-id suffix.
constexpr auto kCatalog = std::to_array<CatalogEntry>({
    {"sub_ecu_denso_sh7058_can_diesel", "SH7058d", 0x00100000, 0xFFFF4000},
    {"sub_ecu_denso_sh7059_can_diesel", "SH7059d", 0x00180000, 0xFFFEE000},
});

Result<CatalogEntry> GetCatalogEntry(std::string_view protocol, std::string_view mcu)
{
    const auto it = std::ranges::find(kCatalog, protocol, &CatalogEntry::protocol);
    if (it == kCatalog.end())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Unsupported Subaru Denso SH705x diesel CAN protocol: {}", protocol));
    }
    if (mcu != it->mcu)
    {
        return Fail(
            ErrorKind::kInvalidConfig,
            std::format("Subaru Denso SH705x diesel CAN protocol {} requires MCU {}, not {}", protocol, it->mcu, mcu));
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

bool WireParametersMatch(const SubaruDensoSh7058CanDieselPlan& wire)
{
    return wire.request_id == 0x7E0 && wire.response_id == 0x7E8 && wire.bitrate == 500000 && !wire.extended_id;
}

Status ValidateImage(const FlashPlan& plan, const FlashDevice& device)
{
    if (plan.Operation() == FlashOperation::kRead)
    {
        return !detail::OperationImageShapeMatches(plan, device.romsize)
                   ? Fail(ErrorKind::kInvalidConfig, "diesel read plans must not carry an image")
                   : Status{};
    }
    if (!detail::OperationImageShapeMatches(plan, device.romsize))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return {};
}

} // namespace

Status ValidateSubaruDensoSh7058CanDieselPlan(const FlashPlan& plan)
{
    const auto entry = GetCatalogEntry(plan.TargetId(), plan.McuName());
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
    if (!plan.Kernel().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel family requires a kernel image");
    }
    if (Status kernel = detail::ValidateKernelUpload<128>(plan.Kernel()->bytes.size(), plan.Kernel()->load_address,
                                                          entry->kernel_load_address, device->kblocks[0]);
        !kernel.has_value())
    {
        return kernel;
    }
    if (!plan.Confirmations().empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel plans must not declare extra confirmations");
    }
    if (Status regions = detail::ValidateRegions(plan, *device); !regions.has_value())
    {
        return regions;
    }
    return ValidateImage(plan, *device);
}

Result<FlashPlan> BuildSubaruDensoSh7058CanDieselPlan(FlashOperation operation, std::string_view protocol_name,
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
        return Fail(ErrorKind::kInvalidConfig, "diesel catalog does not match the flash device table");
    }
    Result<FlashPlanFields> fields = detail::WholeRomKernelPlanFields<128>(
        operation, protocol_name, mcu_type, std::move(image), std::move(kernel), entry->kernel_load_address, *device);
    if (!fields.has_value())
    {
        return std::unexpected(fields.error());
    }
    fields->family = FlashFamily::kSubaruDensoSh7058CanDiesel;
    fields->transport = TransportKind::kCanIso15765;
    fields->family_plan = SubaruDensoSh7058CanDieselPlan{};
    Result<FlashPlan> plan = ValidateAndBuild(std::move(*fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = ValidateSubaruDensoSh7058CanDieselPlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
