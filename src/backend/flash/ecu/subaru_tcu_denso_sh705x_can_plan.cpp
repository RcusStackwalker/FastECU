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

Result<CatalogEntry> GetCatalogEntry(std::string_view protocol, std::string_view mcu)
{
    const auto it = std::ranges::find(kCatalog, protocol, &CatalogEntry::protocol);
    if (it == kCatalog.end())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("Unsupported Denso SH705x TCU CAN protocol: {}", protocol));
    }
    if (mcu != it->mcu)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Denso SH705x TCU CAN protocol {} requires MCU {}, not {}", protocol, it->mcu, mcu));
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

SubaruTcuDensoSh705xCanPlan WireParameters()
{
    return {};
}

bool WireParametersMatch(const SubaruTcuDensoSh705xCanPlan& wire)
{
    return wire.request_id == 0x7E1 && wire.response_id == 0x7E9 && wire.bitrate == 500000 && !wire.extended_id;
}

Status ValidateImage(const FlashPlan& plan, const FlashDevice& device)
{
    if (plan.Operation() == FlashOperation::kRead)
    {
        return !detail::OperationImageShapeMatches(plan, device.romsize)
                   ? Fail(ErrorKind::kInvalidConfig, "TCU read plan carries an image")
                   : Status{};
    }
    if (!detail::OperationImageShapeMatches(plan, device.romsize))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return {};
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
    const auto entry = GetCatalogEntry(plan.TargetId(), plan.McuName());
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
    if (!plan.Kernel().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "TCU requires a kernel image");
    }
    if (Status kernel = detail::ValidateKernelUpload<128>(plan.Kernel()->bytes.size(), plan.Kernel()->load_address,
                                                          entry->kernel_load_address, device->kblocks[0]);
        !kernel.has_value())
    {
        return kernel;
    }
    if (!plan.Confirmations().empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "TCU plans must not declare extra confirmations");
    }
    if (Status regions = detail::ValidateRegions(plan, *device); !regions.has_value())
    {
        return regions;
    }
    return ValidateImage(plan, *device);
}

Result<FlashPlan> BuildSubaruTcuDensoSh705xCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                   KernelImage kernel)
{
    const auto entry = GetCatalogEntry(protocol_name, mcu_type);
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
    Result<FlashPlanFields> fields = detail::WholeRomKernelPlanFields<128>(
        operation, protocol_name, mcu_type, std::move(image), std::move(kernel), entry->kernel_load_address, *device);
    if (!fields.has_value())
    {
        return std::unexpected(fields.error());
    }
    fields->family = FlashFamily::kSubaruTcuDensoSh705xCan;
    fields->transport = TransportKind::kCanIso15765;
    fields->family_plan = WireParameters();
    Result<FlashPlan> plan = ValidateAndBuild(std::move(*fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = ValidateSubaruTcuDensoSh705xCanPlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
