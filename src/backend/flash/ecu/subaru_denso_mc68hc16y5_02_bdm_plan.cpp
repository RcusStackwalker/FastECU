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
// Legacy write_mem() pads the kernel to a multiple of the shared upload
// chunk (:247-250); the executor uploads in chunks of the same size (:361).
constexpr std::size_t kUploadChunk = kSubaruDensoMc68hc16y5_02BdmUploadChunk;

Status ValidateIdentity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol || mcu != kMcu)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("MC68HC16Y5 BDM protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const FlashDevice *device = FindFlashDevice(mcu);
    if (device == nullptr || device->romsize != kRomSize || device->rblocks == nullptr ||
        device->rblocks[0].start != kRam.start || device->rblocks[0].len != kRam.length)
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM memory map is invalid");
    }
    return {};
}

bytes::Bytes PadKernel(bytes::Bytes kernel)
{
    kernel.resize((kernel.size() + kUploadChunk - 1) / kUploadChunk * kUploadChunk, 0x00);
    return kernel;
}
} // namespace

Status ValidateSubaruDensoMc68hc16y502BdmPlan(const FlashPlan& plan)
{
    if (plan.Family() != FlashFamily::kSubaruDensoMc68hc16y502Bdm || plan.Transport() != TransportKind::kKline)
    {
        return Fail(ErrorKind::kInvalidConfig, "plan is not for Subaru Denso MC68HC16Y5 BDM");
    }
    if (auto identity = ValidateIdentity(plan.TargetId(), plan.McuName()); !identity.has_value())
    {
        return identity;
    }
    if (plan.Operation() == FlashOperation::kTestWrite)
    {
        return Fail(ErrorKind::kUnsupported, "MC68HC16Y5 BDM has no test write");
    }
    const auto *wire = std::get_if<SubaruDensoMc68hc16y5_02BdmPlan>(&plan.FamilyPlan());
    if (wire == nullptr || wire->baud != kBaud)
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM wire parameters are invalid");
    }
    if (!plan.EraseRegions().empty() || plan.Kernel().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM plan shape is invalid");
    }
    const auto confirmations = plan.Confirmations();
    const bool exactly_bootstrap = confirmations.size() == 1 &&
                                   confirmations.front().id == ConfirmationSpec::Id::kKernelBootstrap &&
                                   confirmations.front().arguments.empty();
    if (plan.Operation() == FlashOperation::kWrite ? !exactly_bootstrap : !confirmations.empty())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    "MC68HC16Y5 BDM Write requires exactly the KernelBootstrap confirmation; Read requires none");
    }
    if (plan.Operation() == FlashOperation::kRead)
    {
        if (plan.TransferRegion() != kReadRegion || plan.Image().has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM read-plan shape is invalid");
        }
        return {};
    }
    const auto& image = plan.Image();
    if (!image.has_value() || image->empty() || image->size() % kUploadChunk != 0 || image->size() > kRam.length ||
        plan.TransferRegion() != MemoryRegion{kRam.start, static_cast<std::uint32_t>(image->size())})
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM kernel-bootstrap plan shape is invalid");
    }
    return {};
}

Result<FlashPlan> BuildSubaruDensoMc68hc16y502BdmPlan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> rom_image,
                                                      std::optional<KernelImage> kernel)
{
    if (auto identity = ValidateIdentity(protocol_name, mcu_type); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    if (operation == FlashOperation::kTestWrite)
    {
        return Fail(ErrorKind::kUnsupported, "MC68HC16Y5 BDM has no test write");
    }
    if (rom_image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM plans never carry a ROM image");
    }

    MemoryRegion region = kReadRegion;
    std::optional<bytes::Bytes> image;
    if (operation == FlashOperation::kRead)
    {
        if (kernel.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM read plans must not carry a kernel");
        }
    }
    else
    {
        if (!kernel.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM kernel bootstrap requires a kernel image");
        }
        if (kernel->load_address != kRam.start)
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("MC68HC16Y5 BDM kernel must load at 0x{:X}, not 0x{:X}",
                                                               kRam.start, kernel->load_address));
        }
        if (kernel->bytes.empty())
        {
            return Fail(ErrorKind::kInvalidConfig, "MC68HC16Y5 BDM kernel image is empty");
        }
        bytes::Bytes padded = PadKernel(std::move(kernel->bytes));
        if (padded.size() > kRam.length)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("MC68HC16Y5 BDM kernel ({} bytes padded) exceeds the 0x{:X}-byte RAM block",
                                    padded.size(), kRam.length));
        }
        region = MemoryRegion{kRam.start, static_cast<std::uint32_t>(padded.size())};
        image = std::move(padded);
    }

    auto plan = ValidateAndBuild(FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::kSubaruDensoMc68hc16y502Bdm,
        .transport = TransportKind::kKline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = region,
        .erase_regions = {},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = kBaud},
        .confirmations = operation == FlashOperation::kWrite ? std::vector<ConfirmationSpec>{ConfirmationSpec{
                                                                   .id = ConfirmationSpec::Id::kKernelBootstrap}}
                                                             : std::vector<ConfirmationSpec>{},
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = ValidateSubaruDensoMc68hc16y502BdmPlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
