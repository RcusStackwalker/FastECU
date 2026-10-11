#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"

#include <array>
#include <format>
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
// Security selection replaces the legacy suffix chain at operation.cpp:458-477;
// the executor consumes only this enum and never reinterprets target_id().
constexpr std::array<CatalogEntry, 5> kCatalog{{
    {"sub_ecu_denso_sh7058_can", SubaruDensoSh7058CanSecurity::kStock},
    {"sub_ecu_denso_sh7058_can_ecutek", SubaruDensoSh7058CanSecurity::kEcuTek},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", SubaruDensoSh7058CanSecurity::kRaceRom},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", SubaruDensoSh7058CanSecurity::kRaceRomAlt},
    {"sub_ecu_denso_sh7058_can_cobb", SubaruDensoSh7058CanSecurity::kCobb},
}};

constexpr std::string_view kMcu = "SH7058";
constexpr std::uint32_t kRomSize = 0x00100000;
constexpr std::uint32_t kKernelLoadAddress = 0xFFFF3000;

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
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Unsupported Subaru Denso SH7058 petrol CAN protocol: {}", protocol));
    }
    if (mcu != kMcu)
    {
        return Fail(
            ErrorKind::kInvalidConfig,
            std::format("Subaru Denso SH7058 petrol CAN protocol {} requires MCU SH7058, not {}", protocol, mcu));
    }
    return {};
}

const FlashDevice *CheckedDevice()
{
    const FlashDevice *device = FindFlashDevice(kMcu);
    if (device == nullptr || device->romsize != kRomSize || device->numblocks != 16 || device->fblocks == nullptr ||
        device->kblocks == nullptr)
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

Status ValidateImage(const FlashPlan& plan, const FlashDevice& device)
{
    if (plan.Operation() == FlashOperation::kRead)
    {
        return plan.Image().has_value() ? Fail(ErrorKind::kInvalidConfig, "petrol SH7058 read plan carries a ROM image")
                                        : Status{};
    }
    if (!plan.Image().has_value() || plan.Image()->size() != device.romsize)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return {};
}

} // namespace

Status ValidateSubaruDensoSh7058CanPlan(const FlashPlan& plan)
{
    if (plan.Family() != FlashFamily::kSubaruDensoSh7058Can || plan.Transport() != TransportKind::kCanIso15765)
    {
        return Fail(ErrorKind::kInvalidConfig, "plan is not for Subaru Denso SH7058 petrol CAN");
    }
    const CatalogEntry *entry = nullptr;
    if (Status identity = ValidateIdentity(plan.TargetId(), plan.McuName(), entry); !identity.has_value())
    {
        return identity;
    }
    const auto *wire = std::get_if<SubaruDensoSh7058CanPlan>(&plan.FamilyPlan());
    if (wire == nullptr || !WireParametersMatch(*wire, *entry))
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 CAN wire/security parameters are invalid");
    }
    const FlashDevice *device = CheckedDevice();
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 catalog does not match the flash device table");
    }
    if (!plan.Kernel().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 requires a kernel image");
    }
    if (Status kernel = detail::ValidateKernelUpload<128>(plan.Kernel()->bytes.size(), plan.Kernel()->load_address,
                                                          kKernelLoadAddress, device->kblocks[0]);
        !kernel.has_value())
    {
        return kernel;
    }
    if (!plan.Confirmations().empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 plans must not declare extra confirmations");
    }
    if (Status regions = detail::ValidateRegions(plan, *device); !regions.has_value())
    {
        return regions;
    }
    return ValidateImage(plan, *device);
}

Result<FlashPlan> BuildSubaruDensoSh7058CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                KernelImage kernel)
{
    const CatalogEntry *entry = nullptr;
    if (Status identity = ValidateIdentity(protocol_name, mcu_type, entry); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    const FlashDevice *device = CheckedDevice();
    if (device == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 catalog does not match the flash device table");
    }
    if (Status upload = detail::ValidateKernelUpload<128>(kernel.bytes.size(), kernel.load_address, kKernelLoadAddress,
                                                          device->kblocks[0]);
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }
    if (operation == FlashOperation::kRead)
    {
        if (image.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "petrol SH7058 read plans must not carry an image");
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
        .family = FlashFamily::kSubaruDensoSh7058Can,
        .transport = TransportKind::kCanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = std::move(erase_regions),
        .image = operation == FlashOperation::kRead ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = SubaruDensoSh7058CanPlan{.security = entry->security},
        .confirmations = {},
    };
    Result<FlashPlan> plan = ValidateAndBuild(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = ValidateSubaruDensoSh7058CanPlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
