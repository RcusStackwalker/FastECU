#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"

#include <array>
#include <format>
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
constexpr std::array<CatalogEntry, 5> kCatalog{{
    {"sub_ecu_denso_sh7055_densocan", "SH7055", 0x00080000, 0xFFFF6004},
    {"sub_ecu_denso_sh7058_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7058s_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7058s_diesel_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7059_diesel_densocan", "SH7059d", 0x00180000, 0xFFFEE000},
}};

const CatalogEntry *FindCatalog(std::string_view protocol)
{
    for (const CatalogEntry& entry : kCatalog)
    {
        if (entry.protocol == protocol)
        {
            return &entry;
        }
    }
    return nullptr;
}

Status ValidateIdentity(std::string_view protocol, std::string_view mcu, const CatalogEntry *& entry)
{
    entry = FindCatalog(protocol);
    if (entry == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("Unsupported DensoCAN protocol: {}", protocol));
    }
    if (mcu != entry->mcu)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("DensoCAN protocol {} requires MCU {}, not {}", protocol, entry->mcu, mcu));
    }
    return {};
}

const FlashDevice *CheckedDevice(const CatalogEntry& entry)
{
    const FlashDevice *device = FindFlashDevice(entry.mcu);
    if (device == nullptr || device->romsize != entry.rom_size || device->numblocks != 16 ||
        device->fblocks == nullptr || device->kblocks == nullptr)
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

Status ValidateImage(const FlashPlan& plan, const FlashDevice& device)
{
    if (plan.Operation() == FlashOperation::kRead)
    {
        return plan.Image().has_value() ? Fail(ErrorKind::kInvalidConfig, "DensoCAN read plan carries an image")
                                        : Status{};
    }
    if (!plan.Image().has_value() || plan.Image()->size() != device.romsize)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return {};
}

} // namespace

Status ValidateSubaruDensoSh705xDensocanPlan(const FlashPlan& plan)
{
    if (plan.Family() != FlashFamily::kSubaruDensoSh705xDensoCan || plan.Transport() != TransportKind::kCanRawIso15765)
    {
        return Fail(ErrorKind::kInvalidConfig, "plan is not for Subaru Denso SH705x DensoCAN");
    }
    const auto *wire = std::get_if<SubaruDensoSh705xDensoCanPlan>(&plan.FamilyPlan());
    if (wire == nullptr || !WireParametersMatch(*wire))
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN wire parameters are invalid");
    }
    const CatalogEntry *entry = nullptr;
    if (Status identity = ValidateIdentity(plan.TargetId(), plan.McuName(), entry); !identity.has_value())
    {
        return identity;
    }
    const FlashDevice *device = CheckedDevice(*entry);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN catalog does not match the flash device table");
    }
    if (!plan.Kernel().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN requires a kernel image");
    }
    if (Status kernel = detail::ValidateKernelUpload<6>(plan.Kernel()->bytes.size(), plan.Kernel()->load_address,
                                                        entry->kernel_load_address, device->kblocks[0]);
        !kernel.has_value())
    {
        return kernel;
    }
    if (plan.Confirmations().size() != 1 || plan.Confirmations().front().id != ConfirmationSpec::Id::kCycleIgnition ||
        !plan.Confirmations().front().arguments.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN requires exactly the CycleIgnition confirmation");
    }
    if (Status regions = detail::ValidateRegions(plan, *device); !regions.has_value())
    {
        return regions;
    }
    return ValidateImage(plan, *device);
}

Result<FlashPlan> BuildSubaruDensoSh705xDensocanPlan(FlashOperation operation, std::string_view protocol_name,
                                                     std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                     KernelImage kernel)
{
    const CatalogEntry *entry = nullptr;
    if (Status identity = ValidateIdentity(protocol_name, mcu_type, entry); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    const FlashDevice *device = CheckedDevice(*entry);
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "DensoCAN catalog does not match the flash device table");
    }
    if (Status upload = detail::ValidateKernelUpload<6>(kernel.bytes.size(), kernel.load_address,
                                                        entry->kernel_load_address, device->kblocks[0]);
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }
    if (operation == FlashOperation::kRead)
    {
        if (image.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "DensoCAN read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != device->romsize)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device->romsize));
    }

    std::vector<MemoryRegion> erase_regions;
    if (operation != FlashOperation::kRead)
    {
        erase_regions = detail::MakeEraseRegions(*device);
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::kSubaruDensoSh705xDensoCan,
        .transport = TransportKind::kCanRawIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = std::move(erase_regions),
        .image = operation == FlashOperation::kRead ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = WireParameters(),
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}},
    };
    Result<FlashPlan> plan = ValidateAndBuild(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = ValidateSubaruDensoSh705xDensocanPlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
