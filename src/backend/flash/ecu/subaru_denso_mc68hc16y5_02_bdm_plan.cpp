#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"

#include <cstddef>
#include <format>
#include <string_view>
#include <utility>

#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
constexpr std::string_view kProtocol = "sub_ecu_denso_mc68hc16y5_02_bdm";
constexpr std::string_view kMcu = "MC68HC16Y5";
// Legacy execute() :54.
constexpr int kBaud = 115200;
// Legacy read_mem() walks the whole 0x0-0x2FFFF address space, filling the
// RAM block with 0xFF (:116-144), so the read image is physical, not packed.
constexpr MemoryRegion kReadRegion{0, 0x30000};
constexpr MemoryRegion kRam{0x20000, 0x8000};
constexpr std::uint32_t kRomSize = 0x28000;
// Legacy write_mem() pads the kernel to 32 bytes (:247-250) and flash_block()
// uploads it in 32-byte chunks (:361).
constexpr std::size_t kUploadChunk = 0x20;

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol || mcu != kMcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("MC68HC16Y5 BDM protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const flashdev_t *device = find_flash_device(mcu);
    if (device == nullptr || device->romsize != kRomSize || device->rblocks == nullptr ||
        device->rblocks[0].start != kRam.start || device->rblocks[0].len != kRam.length)
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM memory map is invalid");
    }
    return {};
}

bytes::Bytes pad_kernel(bytes::Bytes kernel)
{
    kernel.resize((kernel.size() + kUploadChunk - 1) / kUploadChunk * kUploadChunk, 0x00);
    return kernel;
}
} // namespace

Status validate_subaru_denso_mc68hc16y5_02_bdm_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruDensoMc68hc16y5_02Bdm || plan.transport() != TransportKind::Kline)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Denso MC68HC16Y5 BDM");
    }
    if (auto identity = validate_identity(plan.target_id(), plan.mcu_name()); !identity.has_value())
    {
        return identity;
    }
    if (plan.operation() == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "MC68HC16Y5 BDM has no test write");
    }
    const auto *wire = std::get_if<SubaruDensoMc68hc16y5_02BdmPlan>(&plan.family_plan());
    if (wire == nullptr || wire->baud != kBaud)
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM wire parameters are invalid");
    }
    if (!plan.erase_regions().empty() || plan.kernel().has_value() || !plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM plan shape is invalid");
    }
    if (plan.operation() == FlashOperation::Read)
    {
        if (plan.transfer_region() != kReadRegion || plan.image().has_value())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM read-plan shape is invalid");
        }
        return {};
    }
    const auto& image = plan.image();
    if (!image.has_value() || image->empty() || image->size() % kUploadChunk != 0 || image->size() > kRam.length ||
        plan.transfer_region() != MemoryRegion{kRam.start, static_cast<std::uint32_t>(image->size())})
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM kernel-bootstrap plan shape is invalid");
    }
    return {};
}

Result<FlashPlan> build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation operation, std::string_view protocol_name,
                                                            std::string_view mcu_type,
                                                            std::optional<bytes::Bytes> rom_image,
                                                            std::optional<KernelImage> kernel)
{
    if (auto identity = validate_identity(protocol_name, mcu_type); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "MC68HC16Y5 BDM has no test write");
    }
    if (rom_image.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM plans never carry a ROM image");
    }

    MemoryRegion region = kReadRegion;
    std::optional<bytes::Bytes> image;
    if (operation == FlashOperation::Read)
    {
        if (kernel.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM read plans must not carry a kernel");
        }
    }
    else
    {
        if (!kernel.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM kernel bootstrap requires a kernel image");
        }
        if (kernel->load_address != kRam.start)
        {
            return fail(ErrorKind::InvalidConfig, std::format("MC68HC16Y5 BDM kernel must load at 0x{:X}, not 0x{:X}",
                                                              kRam.start, kernel->load_address));
        }
        if (kernel->bytes.empty())
        {
            return fail(ErrorKind::InvalidConfig, "MC68HC16Y5 BDM kernel image is empty");
        }
        bytes::Bytes padded = pad_kernel(std::move(kernel->bytes));
        if (padded.size() > kRam.length)
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("MC68HC16Y5 BDM kernel ({} bytes padded) exceeds the 0x{:X}-byte RAM block",
                                    padded.size(), kRam.length));
        }
        region = MemoryRegion{kRam.start, static_cast<std::uint32_t>(padded.size())};
        image = std::move(padded);
    }

    auto plan = validate_and_build(FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::SubaruDensoMc68hc16y5_02Bdm,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = region,
        .erase_regions = {},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = kBaud},
        .confirmations = {},
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_denso_mc68hc16y5_02_bdm_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
