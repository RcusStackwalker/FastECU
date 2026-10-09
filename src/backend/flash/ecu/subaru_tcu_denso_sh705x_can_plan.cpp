#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"

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
constexpr std::array<CatalogEntry, 2> kCatalog{{
    {"sub_tcu_denso_sh7055_can", "SH7055", 0x00080000, 0xFFFF9000, false},
    {"sub_tcu_denso_sh7058_can", "SH7058", 0x00100000, 0xFFFF3000, true},
}};

const CatalogEntry *find_catalog(std::string_view protocol)
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

Status validate_identity(std::string_view protocol, std::string_view mcu, const CatalogEntry *& entry)
{
    entry = find_catalog(protocol);
    if (entry == nullptr)
    {
        return fail(ErrorKind::kInvalidConfig, std::format("Unsupported Denso SH705x TCU CAN protocol: {}", protocol));
    }
    if (mcu != entry->mcu)
    {
        return fail(ErrorKind::kInvalidConfig,
                    std::format("Denso SH705x TCU CAN protocol {} requires MCU {}, not {}", protocol, entry->mcu, mcu));
    }
    return {};
}

const FlashDevice *checked_device(const CatalogEntry& entry)
{
    const FlashDevice *device = find_flash_device(entry.mcu);
    if (device == nullptr || device->romsize != entry.rom_size || device->numblocks != 16 ||
        device->fblocks == nullptr || device->kblocks == nullptr)
    {
        return nullptr;
    }
    return device;
}

SubaruTcuDensoSh705xCanPlan wire_parameters()
{
    return {};
}

bool wire_parameters_match(const SubaruTcuDensoSh705xCanPlan& wire)
{
    return wire.request_id == 0x7E1 && wire.response_id == 0x7E9 && wire.bitrate == 500000 && !wire.extended_id;
}

Status validate_image(const FlashPlan& plan, const FlashDevice& device)
{
    if (plan.operation() == FlashOperation::kRead)
    {
        return plan.image().has_value() ? fail(ErrorKind::kInvalidConfig, "TCU read plan carries an image") : Status{};
    }
    if (!plan.image().has_value() || plan.image()->size() != device.romsize)
    {
        return fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return {};
}

Status validate_capability(const FlashPlan& plan, const CatalogEntry& entry)
{
    if (plan.operation() == FlashOperation::kTestWrite ||
        (plan.operation() == FlashOperation::kWrite && !entry.supports_write))
    {
        return fail(ErrorKind::kUnsupported, "operation is not supported by the selected Denso SH705x TCU");
    }
    return {};
}

} // namespace

Status validate_subaru_tcu_denso_sh705x_can_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::kSubaruTcuDensoSh705xCan || plan.transport() != TransportKind::kCanIso15765)
    {
        return fail(ErrorKind::kInvalidConfig, "plan is not for Subaru Denso SH705x TCU CAN");
    }
    const auto *wire = std::get_if<SubaruTcuDensoSh705xCanPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_parameters_match(*wire))
    {
        return fail(ErrorKind::kInvalidConfig, "TCU CAN wire parameters are invalid");
    }
    const CatalogEntry *entry = nullptr;
    if (Status identity = validate_identity(plan.target_id(), plan.mcu_name(), entry); !identity.has_value())
    {
        return identity;
    }
    if (Status capability = validate_capability(plan, *entry); !capability.has_value())
    {
        return capability;
    }
    const FlashDevice *device = checked_device(*entry);
    if (device == nullptr)
    {
        return fail(ErrorKind::kInvalidConfig, "TCU catalog does not match the flash device table");
    }
    if (!plan.kernel().has_value())
    {
        return fail(ErrorKind::kInvalidConfig, "TCU requires a kernel image");
    }
    if (Status kernel = detail::validate_kernel_upload<128>(plan.kernel()->bytes.size(), plan.kernel()->load_address,
                                                            entry->kernel_load_address, device->kblocks[0]);
        !kernel.has_value())
    {
        return kernel;
    }
    if (!plan.confirmations().empty())
    {
        return fail(ErrorKind::kInvalidConfig, "TCU plans must not declare extra confirmations");
    }
    if (Status regions = detail::validate_regions(plan, *device); !regions.has_value())
    {
        return regions;
    }
    return validate_image(plan, *device);
}

Result<FlashPlan> build_subaru_tcu_denso_sh705x_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                         std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                         KernelImage kernel)
{
    const CatalogEntry *entry = nullptr;
    if (Status identity = validate_identity(protocol_name, mcu_type, entry); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    // Capability is checked before image/kernel validation, preserving the
    // SH7055 write and all test-write rejections before any hardware path.
    if (operation == FlashOperation::kTestWrite || (operation == FlashOperation::kWrite && !entry->supports_write))
    {
        return fail(ErrorKind::kUnsupported, "operation is not supported by the selected Denso SH705x TCU");
    }
    const FlashDevice *device = checked_device(*entry);
    if (device == nullptr)
    {
        return fail(ErrorKind::kInvalidConfig, "TCU catalog does not match the flash device table");
    }
    if (Status upload = detail::validate_kernel_upload<128>(kernel.bytes.size(), kernel.load_address,
                                                            entry->kernel_load_address, device->kblocks[0]);
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }
    if (operation == FlashOperation::kRead)
    {
        if (image.has_value())
        {
            return fail(ErrorKind::kInvalidConfig, "TCU read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != device->romsize)
    {
        return fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device->romsize));
    }

    std::vector<MemoryRegion> erase_regions;
    if (operation == FlashOperation::kWrite)
    {
        erase_regions = detail::make_erase_regions(*device);
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::kSubaruTcuDensoSh705xCan,
        .transport = TransportKind::kCanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = std::move(erase_regions),
        .image = operation == FlashOperation::kRead ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = wire_parameters(),
        .confirmations = {},
    };
    Result<FlashPlan> plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_subaru_tcu_denso_sh705x_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
