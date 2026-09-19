#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"

#include <array>
#include <format>
#include <limits>
#include <utility>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_types.h"
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

// Exact protocols.cfg/legacy catalogue. These are intentionally exact
// identities rather than a suffix rule: EEPROM DensoCAN and future variants
// are a different family until designed and tested explicitly.
constexpr std::array<CatalogEntry, 5> kCatalog{{
    {"sub_ecu_denso_sh7055_densocan", "SH7055", 0x00080000, 0xFFFF6004},
    {"sub_ecu_denso_sh7058_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7058s_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7058s_diesel_densocan", "SH7058", 0x00100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7059_diesel_densocan", "SH7059d", 0x00180000, 0xFFFEE000},
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
        return fail(ErrorKind::InvalidConfig, std::format("Unsupported DensoCAN protocol: {}", protocol));
    }
    if (mcu != entry->mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("DensoCAN protocol {} requires MCU {}, not {}", protocol, entry->mcu, mcu));
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
    // Legacy upload_kernel() at lines 227-507 sends six-byte raw CAN blocks.
    // The last block is zero padded, so the physical upload length, rather
    // than the caller's byte count, must fit the model's kernel region.
    if (kernel.load_address != entry.kernel_load_address)
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN kernel address does not match the selected protocol");
    }
    const std::uint64_t size = kernel.bytes.size();
    if (size > std::numeric_limits<std::uint64_t>::max() - 5U)
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN kernel size cannot be padded to six-byte blocks");
    }
    const std::uint64_t padded_size = ((size + 5U) / 6U) * 6U;
    const std::uint64_t region_start = device.kblocks[0].start;
    const std::uint64_t region_end = region_start + device.kblocks[0].len;
    const std::uint64_t upload_start = kernel.load_address;
    if (upload_start < region_start || upload_start > region_end || padded_size > region_end - upload_start)
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN padded kernel is outside the selected MCU kernel region");
    }
    return {};
}

SubaruDensoSh705xDensoCanPlan wire_parameters()
{
    return {};
}

bool wire_parameters_match(const SubaruDensoSh705xDensoCanPlan& wire)
{
    return wire.iso_request_id == 0x7E0 && wire.iso_response_id == 0x7E8 && wire.raw_transmit_id == 0x000FFFFE &&
           wire.raw_receive_id == 0x21 && wire.bitrate == 500000 && !wire.iso_extended_id && wire.raw_extended_id;
}

Status validate_regions(const FlashPlan& plan, const flashdev_t& device)
{
    if (plan.transfer_region().start != device.fblocks[0].start || plan.transfer_region().length != device.romsize)
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN transfer region does not match the MCU");
    }
    if (plan.operation() == FlashOperation::Read)
    {
        if (!plan.erase_regions().empty())
        {
            return fail(ErrorKind::InvalidConfig, "DensoCAN read plans must not declare erase regions");
        }
        return {};
    }
    if (plan.erase_regions().size() != device.numblocks)
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN write plans must declare every flash block");
    }
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion expected{device.fblocks[index].start, device.fblocks[index].len};
        const MemoryRegion actual = plan.erase_regions()[index];
        if (actual.start != expected.start || actual.length != expected.length)
        {
            return fail(ErrorKind::InvalidConfig, "DensoCAN erase geometry does not match the MCU");
        }
    }
    return {};
}

Status validate_image(const FlashPlan& plan, const flashdev_t& device)
{
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.image().has_value() ? fail(ErrorKind::InvalidConfig, "DensoCAN read plan carries an image")
                                        : Status{};
    }
    if (!plan.image().has_value() || plan.image()->size() != device.romsize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return {};
}

} // namespace

Status validate_subaru_denso_sh705x_densocan_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruDensoSh705xDensoCan || plan.transport() != TransportKind::CanRawIso15765)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Denso SH705x DensoCAN");
    }
    const auto *wire = std::get_if<SubaruDensoSh705xDensoCanPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_parameters_match(*wire))
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN wire parameters are invalid");
    }
    const CatalogEntry *entry = nullptr;
    if (Status identity = validate_identity(plan.target_id(), plan.mcu_name(), entry); !identity.has_value())
    {
        return identity;
    }
    const flashdev_t *device = checked_device(*entry);
    if (device == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN catalog does not match the flash device table");
    }
    if (!plan.kernel().has_value())
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN requires a kernel image");
    }
    if (Status kernel = validate_kernel_upload(*plan.kernel(), *entry, *device); !kernel.has_value())
    {
        return kernel;
    }
    if (plan.confirmations().size() != 1 || plan.confirmations().front().id != ConfirmationSpec::Id::CycleIgnition ||
        !plan.confirmations().front().arguments.empty())
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN requires exactly the CycleIgnition confirmation");
    }
    if (Status regions = validate_regions(plan, *device); !regions.has_value())
    {
        return regions;
    }
    return validate_image(plan, *device);
}

Result<FlashPlan> build_subaru_denso_sh705x_densocan_plan(FlashOperation operation, std::string_view protocol_name,
                                                          std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                          KernelImage kernel)
{
    const CatalogEntry *entry = nullptr;
    if (Status identity = validate_identity(protocol_name, mcu_type, entry); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    const flashdev_t *device = checked_device(*entry);
    if (device == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN catalog does not match the flash device table");
    }
    if (Status upload = validate_kernel_upload(kernel, *entry, *device); !upload.has_value())
    {
        return std::unexpected(upload.error());
    }
    if (operation == FlashOperation::Read)
    {
        if (image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "DensoCAN read plans must not carry an image");
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
        .family = FlashFamily::SubaruDensoSh705xDensoCan,
        .transport = TransportKind::CanRawIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = std::move(erase_regions),
        .image = operation == FlashOperation::Read ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = wire_parameters(),
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}},
    };
    Result<FlashPlan> plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_densocan_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
