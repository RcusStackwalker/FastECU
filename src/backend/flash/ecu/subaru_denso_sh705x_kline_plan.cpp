#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan_detail.h"

#include <array>
#include <format>
#include <utility>

#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
using enum ErrorKind;

// The exact protocols the legacy MainWindow branches reached:
// startsWith("sub_ecu_denso_sh7055_04") (three cfg entries today) and the
// three named sh7058 protocols. Listing them closes the prefix to lookalikes.
struct Variant
{
    std::string_view protocol;
    std::string_view mcu;
    SubaruDensoSh705xKlineSeedKey seed_key;
    // cfg read=no, test_write=yes, write=no: enforced here, not only by the UI.
    bool test_write_only;
    // cfg <kernel_addr>.
    std::uint32_t kernel_address;
};

constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_denso_sh7055_04", "SH7055", SubaruDensoSh705xKlineSeedKey::kStock, false, 0xFFFF6004},
    {"sub_ecu_denso_sh7055_04_ecutek", "SH7055", SubaruDensoSh705xKlineSeedKey::kEcuTek, false, 0xFFFF6004},
    {"sub_ecu_denso_sh7055_04_cobb", "SH7055", SubaruDensoSh705xKlineSeedKey::kStock, true, 0xFFFF6004},
    {"sub_ecu_denso_sh7058", "SH7058", SubaruDensoSh705xKlineSeedKey::kStock, false, 0xFFFF3000},
    {"sub_ecu_denso_sh7058_ecutek", "SH7058", SubaruDensoSh705xKlineSeedKey::kEcuTek, false, 0xFFFF3000},
    {"sub_ecu_denso_sh7058_cobb", "SH7058", SubaruDensoSh705xKlineSeedKey::kStock, true, 0xFFFF3000},
});

constexpr std::uint32_t kCommitBlockSize = 0x1000;   // flash_block() flashblocksize
constexpr std::uint64_t kMaxWireLength = 0x00FFFFFF; // send_sid_34_request_upload() 24-bit length

Result<const Variant *> FindVariant(std::string_view protocol, std::string_view mcu)
{
    for (const Variant& variant : kVariants)
    {
        if (variant.protocol == protocol)
        {
            if (variant.mcu != mcu)
            {
                return Fail(kInvalidConfig, std::format("{} requires MCU {}, not {}", protocol, variant.mcu, mcu));
            }
            return &variant;
        }
    }
    return Fail(kInvalidConfig, std::format("Unsupported Denso SH705x K-Line protocol: {}", protocol));
}

Status ValidateOperation(const Variant& variant, FlashOperation operation)
{
    if (variant.test_write_only && operation != FlashOperation::kTestWrite)
    {
        return Fail(kUnsupported, std::format("{} supports test write only", variant.protocol));
    }
    return {};
}

Status ValidateKernel(const Variant& variant, const KernelImage& kernel)
{
    if (kernel.bytes.empty())
    {
        return Fail(kInvalidConfig, "Denso SH705x K-Line kernel is empty");
    }
    // upload_kernel(): +2 bytes, padded to 4 -- the encrypted length must fit
    // send_sid_34_request_upload()'s 24-bit length field.
    const std::uint64_t padded = (static_cast<std::uint64_t>(kernel.bytes.size()) + 2 + 3) & ~3ULL;
    if (padded > kMaxWireLength)
    {
        return Fail(kInvalidConfig, "Denso SH705x K-Line kernel exceeds the 24-bit upload length");
    }
    if (kernel.load_address != variant.kernel_address)
    {
        return Fail(kInvalidConfig,
                    std::format("Denso SH705x K-Line kernel address must be 0x{:08X}", variant.kernel_address));
    }
    return {};
}

Status ValidateImage(FlashOperation operation, const std::optional<bytes::Bytes>& image, std::uint32_t romsize)
{
    if (operation == FlashOperation::kRead)
    {
        return {};
    }
    // Correction: legacy write_mem() indexed FullRomData unchecked.
    if (!image.has_value() || image->size() != romsize)
    {
        return Fail(kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }
    return {};
}

} // namespace

namespace detail
{
Status ValidateSubaruDensoSh705xKlineGeometry(const FlashDevice& device)
{
    using enum ErrorKind;
    // Correction: flash_block() loops `remain -= 0x200` and
    // commits at 0x1000 boundaries, and reflash_block() indexes the image by
    // physical address from fblocks[0]. Reject a table that breaks either.
    if (device.numblocks == 0 || device.fblocks[0].start != 0)
    {
        return Fail(kInvalidConfig, "Denso SH705x K-Line flash blocks must start at address 0");
    }
    std::uint64_t total = 0;
    for (unsigned i = 0; i < device.numblocks; ++i)
    {
        if (device.fblocks[i].len % kCommitBlockSize != 0)
        {
            return Fail(kInvalidConfig, "Denso SH705x K-Line flash block is not a multiple of 0x1000");
        }
        total += device.fblocks[i].len;
    }
    if (total != device.romsize)
    {
        return Fail(kInvalidConfig, "Denso SH705x K-Line flash blocks do not cover the ROM");
    }
    return {};
}
} // namespace detail

Status ValidateSubaruDensoSh705xKlinePlan(const FlashPlan& plan)
{
    // Ruling 2: this function is outside the
    // anonymous namespace above, so the `using enum ErrorKind;` there does
    // not reach here -- reintroduce it locally rather than qualifying every
    // use.
    using enum ErrorKind;
    if (plan.Family() != FlashFamily::kSubaruDensoSh705xKline || plan.Transport() != TransportKind::kKline)
    {
        return Fail(kInvalidConfig, "plan is not for Denso SH705x K-Line");
    }
    Result<const Variant *> variant = FindVariant(plan.TargetId(), plan.McuName());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status valid = ValidateOperation(**variant, plan.Operation()); !valid.has_value())
    {
        return valid;
    }
    const auto *family = std::get_if<SubaruDensoSh705xKlinePlan>(&plan.FamilyPlan());
    if (family == nullptr || family->initial_baud != 4800 || family->tester_id != 0xF0 || family->target_id != 0x10 ||
        family->seed_key != (*variant)->seed_key)
    {
        return Fail(kInvalidConfig, "Denso SH705x K-Line wire parameters are invalid");
    }
    if (!plan.EraseRegions().empty() || !plan.Confirmations().empty())
    {
        return Fail(kInvalidConfig, "Denso SH705x K-Line plans carry no erase regions or confirmations");
    }
    if (!plan.Kernel().has_value())
    {
        return Fail(kInvalidConfig, "Denso SH705x K-Line requires a kernel image");
    }
    if (Status valid = ValidateKernel(**variant, *plan.Kernel()); !valid.has_value())
    {
        return valid;
    }
    const FlashDevice *device = FindFlashDevice(plan.McuName());
    if (device == nullptr)
    {
        return Fail(kInvalidConfig, "Unknown MCU type");
    }
    if (Status valid = detail::ValidateSubaruDensoSh705xKlineGeometry(*device); !valid.has_value())
    {
        return valid;
    }
    if (plan.TransferRegion().start != 0 || plan.TransferRegion().length != device->romsize)
    {
        return Fail(kInvalidConfig, "Denso SH705x K-Line transfer region does not match the MCU");
    }
    return ValidateImage(plan.Operation(), plan.Image(), device->romsize);
}

Result<FlashPlan> BuildSubaruDensoSh705xKlinePlan(FlashOperation operation, std::string_view protocol_name,
                                                  std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                  KernelImage kernel)
{
    // See the note in validate_subaru_denso_sh705x_kline_plan above: this
    // function is likewise outside the anonymous namespace.
    using enum ErrorKind;
    Result<const Variant *> variant = FindVariant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status valid = ValidateOperation(**variant, operation); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (Status valid = ValidateKernel(**variant, kernel); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const FlashDevice *device = FindFlashDevice(mcu_type);
    if (device == nullptr)
    {
        return Fail(kInvalidConfig, "Unknown MCU type");
    }
    if (Status valid = detail::ValidateSubaruDensoSh705xKlineGeometry(*device); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (Status valid = ValidateImage(operation, image, device->romsize); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::kSubaruDensoSh705xKline,
        .transport = TransportKind::kKline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = MemoryRegion{0, device->romsize},
        .erase_regions = {},
        .image = operation == FlashOperation::kRead ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan =
            SubaruDensoSh705xKlinePlan{
                .initial_baud = 4800, .tester_id = 0xF0, .target_id = 0x10, .seed_key = (*variant)->seed_key},
        .confirmations = {},
    };
    auto plan = ValidateAndBuild(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = ValidateSubaruDensoSh705xKlinePlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
