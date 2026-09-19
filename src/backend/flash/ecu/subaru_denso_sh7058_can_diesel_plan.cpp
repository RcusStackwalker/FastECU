#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"

#include <array>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

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

// Exact identities from resources/shared/config/protocols.cfg and the diesel
// legacy operation's EURO4/EURO5 comments at revision 59f4e442:113-116. The
// executor consumes the selected plan and never derives a generation from a
// target-id suffix.
constexpr std::array<CatalogEntry, 2> kCatalog{{
    {"sub_ecu_denso_sh7058_can_diesel", "SH7058d", 0x00100000, 0xFFFF4000},
    {"sub_ecu_denso_sh7059_can_diesel", "SH7059d", 0x00180000, 0xFFFEE000},
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
        return fail(ErrorKind::InvalidConfig,
                    std::format("Unsupported Subaru Denso SH705x diesel CAN protocol: {}", protocol));
    }
    if (mcu != entry->mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Subaru Denso SH705x diesel CAN protocol {} requires MCU {}, not {}", protocol,
                                entry->mcu, mcu));
    }
    return {};
}

const flashdev_t *checked_device(const CatalogEntry& entry)
{
    const flashdev_t *device = find_flash_device(entry.mcu);
    if (device == nullptr || device->romsize != entry.rom_size || device->numblocks != 16 ||
        device->fblocks == nullptr || device->kblocks == nullptr)
    {
        return nullptr;
    }
    return device;
}

Status validate_kernel_upload(const KernelImage& kernel, const CatalogEntry& entry, const flashdev_t& device)
{
    // upload_kernel(), revision 59f4e442:517-588, pads to complete 128-byte
    // blocks before uploading. Validate the physical padded range rather than
    // only the caller's unpadded snapshot.
    if (kernel.load_address != entry.kernel_load_address)
    {
        return fail(ErrorKind::InvalidConfig, "diesel kernel address does not match the selected protocol");
    }
    if (kernel.bytes.size() > std::numeric_limits<std::size_t>::max() - 127U)
    {
        return fail(ErrorKind::InvalidConfig, "diesel kernel size cannot be padded to 128-byte blocks");
    }
    const std::size_t padded_size = ((kernel.bytes.size() + 127U) / 128U) * 128U;
    const std::uint64_t region_start = device.kblocks[0].start;
    const std::uint64_t region_end = region_start + device.kblocks[0].len;
    const std::uint64_t upload_start = kernel.load_address;
    if (upload_start < region_start || upload_start > region_end || padded_size > region_end - upload_start)
    {
        return fail(ErrorKind::InvalidConfig, "diesel padded kernel is outside the selected MCU kernel region");
    }
    return {};
}

bool wire_parameters_match(const SubaruDensoSh7058CanDieselPlan& wire)
{
    return wire.request_id == 0x7E0 && wire.response_id == 0x7E8 && wire.bitrate == 500000 && !wire.extended_id;
}

Status validate_regions(const FlashPlan& plan, const flashdev_t& device)
{
    if (plan.transfer_region().start != device.fblocks[0].start || plan.transfer_region().length != device.romsize)
    {
        return fail(ErrorKind::InvalidConfig, "diesel transfer region does not match the selected MCU");
    }
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.erase_regions().empty()
                   ? Status{}
                   : fail(ErrorKind::InvalidConfig, "diesel read plans must not declare erase regions");
    }
    if (plan.erase_regions().size() != device.numblocks)
    {
        return fail(ErrorKind::InvalidConfig, "diesel write plans must declare all 16 flash blocks");
    }
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion expected{device.fblocks[index].start, device.fblocks[index].len};
        const MemoryRegion actual = plan.erase_regions()[index];
        if (actual.start != expected.start || actual.length != expected.length)
        {
            return fail(ErrorKind::InvalidConfig, "diesel erase geometry does not match the selected MCU");
        }
    }
    return {};
}

Status validate_image(const FlashPlan& plan, const flashdev_t& device)
{
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.image().has_value() ? fail(ErrorKind::InvalidConfig, "diesel read plans must not carry an image")
                                        : Status{};
    }
    if (!plan.image().has_value() || plan.image()->size() != device.romsize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return {};
}

} // namespace

Status validate_subaru_denso_sh7058_can_diesel_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruDensoSh7058CanDiesel || plan.transport() != TransportKind::CanIso15765)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Denso SH7058/SH7059 diesel CAN");
    }
    const CatalogEntry *entry = nullptr;
    if (Status identity = validate_identity(plan.target_id(), plan.mcu_name(), entry); !identity.has_value())
    {
        return identity;
    }
    const auto *wire = std::get_if<SubaruDensoSh7058CanDieselPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_parameters_match(*wire))
    {
        return fail(ErrorKind::InvalidConfig, "diesel CAN wire parameters are invalid");
    }
    const flashdev_t *device = checked_device(*entry);
    if (device == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "diesel catalog does not match the flash device table");
    }
    if (!plan.kernel().has_value())
    {
        return fail(ErrorKind::InvalidConfig, "diesel family requires a kernel image");
    }
    if (Status kernel = validate_kernel_upload(*plan.kernel(), *entry, *device); !kernel.has_value())
    {
        return kernel;
    }
    if (!plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "diesel plans must not declare extra confirmations");
    }
    if (Status regions = validate_regions(plan, *device); !regions.has_value())
    {
        return regions;
    }
    return validate_image(plan, *device);
}

Result<FlashPlan> build_subaru_denso_sh7058_can_diesel_plan(FlashOperation operation, std::string_view protocol_name,
                                                            std::string_view mcu_type,
                                                            std::optional<bytes::Bytes> image, KernelImage kernel)
{
    const CatalogEntry *entry = nullptr;
    if (Status identity = validate_identity(protocol_name, mcu_type, entry); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    const flashdev_t *device = checked_device(*entry);
    if (device == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "diesel catalog does not match the flash device table");
    }
    if (Status upload = validate_kernel_upload(kernel, *entry, *device); !upload.has_value())
    {
        return std::unexpected(upload.error());
    }
    if (operation == FlashOperation::Read)
    {
        if (image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "diesel read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != device->romsize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device->romsize));
    }

    std::vector<MemoryRegion> erase_regions;
    if (operation != FlashOperation::Read)
    {
        erase_regions.reserve(device->numblocks);
        for (unsigned index = 0; index < device->numblocks; ++index)
        {
            erase_regions.push_back({device->fblocks[index].start, device->fblocks[index].len});
        }
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh7058CanDiesel,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = std::move(erase_regions),
        .image = operation == FlashOperation::Read ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = SubaruDensoSh7058CanDieselPlan{},
        .confirmations = {},
    };
    Result<FlashPlan> plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_subaru_denso_sh7058_can_diesel_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
