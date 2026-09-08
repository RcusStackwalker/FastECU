#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"

#include <array>
#include <format>
#include <limits>
#include <utility>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_types.h"
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
    bool supports_write;
};

// Exact TCU identities and kernel addresses from protocols.cfg and
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
        return fail(ErrorKind::InvalidConfig, std::format("Unsupported Denso SH705x TCU CAN protocol: {}", protocol));
    }
    if (mcu != entry->mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Denso SH705x TCU CAN protocol {} requires MCU {}, not {}", protocol, entry->mcu, mcu));
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
    // Legacy upload_kernel() at 59f4e442:369-627 rounds the image through
    // 128-byte transfer blocks. The padded physical upload, not merely the
    // unpadded caller bytes, must stay inside the selected kernel region.
    if (kernel.load_address != entry.kernel_load_address)
    {
        return fail(ErrorKind::InvalidConfig, "TCU kernel address does not match the selected protocol");
    }
    const std::uint64_t size = kernel.bytes.size();
    if (size > std::numeric_limits<std::uint64_t>::max() - 127U)
    {
        return fail(ErrorKind::InvalidConfig, "TCU kernel size cannot be padded to 128-byte blocks");
    }
    const std::uint64_t padded_size = ((size + 127U) / 128U) * 128U;
    const std::uint64_t region_start = device.kblocks[0].start;
    const std::uint64_t region_end = region_start + device.kblocks[0].len;
    const std::uint64_t upload_start = kernel.load_address;
    if (upload_start < region_start || upload_start > region_end || padded_size > region_end - upload_start)
    {
        return fail(ErrorKind::InvalidConfig, "TCU padded kernel is outside the selected MCU kernel region");
    }
    return {};
}

SubaruTcuDensoSh705xCanPlan wire_parameters()
{
    return {};
}

bool wire_parameters_match(const SubaruTcuDensoSh705xCanPlan& wire)
{
    return wire.request_id == 0x7E1 && wire.response_id == 0x7E9 && wire.bitrate == 500000 && !wire.extended_id;
}

Status validate_regions(const FlashPlan& plan, const flashdev_t& device)
{
    if (plan.transfer_region().start != device.fblocks[0].start || plan.transfer_region().length != device.romsize)
    {
        return fail(ErrorKind::InvalidConfig, "TCU transfer region does not match the MCU");
    }
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.erase_regions().empty()
                   ? Status{}
                   : fail(ErrorKind::InvalidConfig, "TCU read plans must not declare erase regions");
    }
    if (plan.erase_regions().size() != device.numblocks)
    {
        return fail(ErrorKind::InvalidConfig, "TCU write plans must declare every flash block");
    }
    for (unsigned index = 0; index < device.numblocks; ++index)
    {
        const MemoryRegion expected{device.fblocks[index].start, device.fblocks[index].len};
        const MemoryRegion actual = plan.erase_regions()[index];
        if (actual.start != expected.start || actual.length != expected.length)
        {
            return fail(ErrorKind::InvalidConfig, "TCU erase geometry does not match the MCU");
        }
    }
    return {};
}

Status validate_image(const FlashPlan& plan, const flashdev_t& device)
{
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.image().has_value() ? fail(ErrorKind::InvalidConfig, "TCU read plan carries an image") : Status{};
    }
    if (!plan.image().has_value() || plan.image()->size() != device.romsize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device.romsize));
    }
    return {};
}

Status validate_capability(const FlashPlan& plan, const CatalogEntry& entry)
{
    if (plan.operation() == FlashOperation::TestWrite ||
        (plan.operation() == FlashOperation::Write && !entry.supports_write))
    {
        return fail(ErrorKind::Unsupported, "operation is not supported by the selected Denso SH705x TCU");
    }
    return {};
}

} // namespace

Status validate_subaru_tcu_denso_sh705x_can_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruTcuDensoSh705xCan || plan.transport() != TransportKind::CanIso15765)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Denso SH705x TCU CAN");
    }
    const auto *wire = std::get_if<SubaruTcuDensoSh705xCanPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_parameters_match(*wire))
    {
        return fail(ErrorKind::InvalidConfig, "TCU CAN wire parameters are invalid");
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
    const flashdev_t *device = checked_device(*entry);
    if (device == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "TCU catalog does not match the flash device table");
    }
    if (!plan.kernel().has_value())
    {
        return fail(ErrorKind::InvalidConfig, "TCU requires a kernel image");
    }
    if (Status kernel = validate_kernel_upload(*plan.kernel(), *entry, *device); !kernel.has_value())
    {
        return kernel;
    }
    if (!plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "TCU plans must not declare extra confirmations");
    }
    if (Status regions = validate_regions(plan, *device); !regions.has_value())
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
    if (operation == FlashOperation::TestWrite || (operation == FlashOperation::Write && !entry->supports_write))
    {
        return fail(ErrorKind::Unsupported, "operation is not supported by the selected Denso SH705x TCU");
    }
    const flashdev_t *device = checked_device(*entry);
    if (device == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "TCU catalog does not match the flash device table");
    }
    if (Status upload = validate_kernel_upload(kernel, *entry, *device); !upload.has_value())
    {
        return std::unexpected(upload.error());
    }
    if (operation == FlashOperation::Read)
    {
        if (image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "TCU read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != device->romsize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", device->romsize));
    }

    std::vector<MemoryRegion> erase_regions;
    if (operation == FlashOperation::Write)
    {
        erase_regions.reserve(device->numblocks);
        for (unsigned index = 0; index < device->numblocks; ++index)
        {
            erase_regions.push_back({device->fblocks[index].start, device->fblocks[index].len});
        }
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruTcuDensoSh705xCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = std::move(erase_regions),
        .image = operation == FlashOperation::Read ? std::nullopt : std::move(image),
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
