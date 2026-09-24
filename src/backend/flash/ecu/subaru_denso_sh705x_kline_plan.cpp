#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"

#include <array>
#include <format>
#include <utility>

#include "src/backend/definitions/kernelmemorymodels.h"
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
    {"sub_ecu_denso_sh7055_04", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock, false, 0xFFFF6004},
    {"sub_ecu_denso_sh7055_04_ecutek", "SH7055", SubaruDensoSh705xKlineSeedKey::EcuTek, false, 0xFFFF6004},
    {"sub_ecu_denso_sh7055_04_cobb", "SH7055", SubaruDensoSh705xKlineSeedKey::Stock, true, 0xFFFF6004},
    {"sub_ecu_denso_sh7058", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock, false, 0xFFFF3000},
    {"sub_ecu_denso_sh7058_ecutek", "SH7058", SubaruDensoSh705xKlineSeedKey::EcuTek, false, 0xFFFF3000},
    {"sub_ecu_denso_sh7058_cobb", "SH7058", SubaruDensoSh705xKlineSeedKey::Stock, true, 0xFFFF3000},
});

constexpr std::uint32_t kCommitBlockSize = 0x1000;   // flash_block() flashblocksize
constexpr std::uint64_t kMaxWireLength = 0x00FFFFFF; // send_sid_34_request_upload() 24-bit length

Result<const Variant *> find_variant(std::string_view protocol, std::string_view mcu)
{
    for (const Variant& variant : kVariants)
    {
        if (variant.protocol == protocol)
        {
            if (variant.mcu != mcu)
            {
                return fail(InvalidConfig, std::format("{} requires MCU {}, not {}", protocol, variant.mcu, mcu));
            }
            return &variant;
        }
    }
    return fail(InvalidConfig, std::format("Unsupported Denso SH705x K-Line protocol: {}", protocol));
}

Status validate_operation(const Variant& variant, FlashOperation operation)
{
    if (variant.test_write_only && operation != FlashOperation::TestWrite)
    {
        return fail(Unsupported, std::format("{} supports test write only", variant.protocol));
    }
    return {};
}

Status validate_kernel(const Variant& variant, const KernelImage& kernel)
{
    if (kernel.bytes.empty())
    {
        return fail(InvalidConfig, "Denso SH705x K-Line kernel is empty");
    }
    // upload_kernel(): +2 bytes, padded to 4 -- the encrypted length must fit
    // send_sid_34_request_upload()'s 24-bit length field.
    const std::uint64_t padded = (static_cast<std::uint64_t>(kernel.bytes.size()) + 2 + 3) & ~3ULL;
    if (padded > kMaxWireLength)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line kernel exceeds the 24-bit upload length");
    }
    if (kernel.load_address != variant.kernel_address)
    {
        return fail(InvalidConfig,
                    std::format("Denso SH705x K-Line kernel address must be 0x{:08X}", variant.kernel_address));
    }
    return {};
}

Status validate_geometry(const flashdev_t& device)
{
    // Correction (wave 6b-2): flash_block() loops `remain -= 0x200` and
    // commits at 0x1000 boundaries, and reflash_block() indexes the image by
    // physical address from fblocks[0]. Reject a table that breaks either.
    if (device.numblocks == 0 || device.fblocks[0].start != 0)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line flash blocks must start at address 0");
    }
    std::uint64_t total = 0;
    for (unsigned i = 0; i < device.numblocks; ++i)
    {
        if (device.fblocks[i].len % kCommitBlockSize != 0)
        {
            return fail(InvalidConfig, "Denso SH705x K-Line flash block is not a multiple of 0x1000");
        }
        total += device.fblocks[i].len;
    }
    if (total != device.romsize)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line flash blocks do not cover the ROM");
    }
    return {};
}

Status validate_image(FlashOperation operation, const std::optional<bytes::Bytes>& image, std::uint32_t romsize)
{
    if (operation == FlashOperation::Read)
    {
        return {};
    }
    // Correction (wave 6b-2): legacy write_mem() indexed FullRomData unchecked.
    if (!image.has_value() || image->size() != romsize)
    {
        return fail(InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }
    return {};
}

} // namespace

Status validate_subaru_denso_sh705x_kline_plan(const FlashPlan& plan)
{
    // Ruling 2 (wave 6b-2 controller): this function is outside the
    // anonymous namespace above, so the `using enum ErrorKind;` there does
    // not reach here -- reintroduce it locally rather than qualifying every
    // use.
    using enum ErrorKind;
    if (plan.family() != FlashFamily::SubaruDensoSh705xKline || plan.transport() != TransportKind::Kline)
    {
        return fail(InvalidConfig, "plan is not for Denso SH705x K-Line");
    }
    Result<const Variant *> variant = find_variant(plan.target_id(), plan.mcu_name());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status valid = validate_operation(**variant, plan.operation()); !valid.has_value())
    {
        return valid;
    }
    const auto *family = std::get_if<SubaruDensoSh705xKlinePlan>(&plan.family_plan());
    if (family == nullptr || family->initial_baud != 4800 || family->tester_id != 0xF0 || family->target_id != 0x10 ||
        family->seed_key != (*variant)->seed_key)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line wire parameters are invalid");
    }
    if (!plan.erase_regions().empty() || !plan.confirmations().empty())
    {
        return fail(InvalidConfig, "Denso SH705x K-Line plans carry no erase regions or confirmations");
    }
    if (!plan.kernel().has_value())
    {
        return fail(InvalidConfig, "Denso SH705x K-Line requires a kernel image");
    }
    if (Status valid = validate_kernel(**variant, *plan.kernel()); !valid.has_value())
    {
        return valid;
    }
    const flashdev_t *device = find_flash_device(plan.mcu_name());
    if (device == nullptr)
    {
        return fail(InvalidConfig, "Unknown MCU type");
    }
    if (Status valid = validate_geometry(*device); !valid.has_value())
    {
        return valid;
    }
    if (plan.transfer_region().start != 0 || plan.transfer_region().length != device->romsize)
    {
        return fail(InvalidConfig, "Denso SH705x K-Line transfer region does not match the MCU");
    }
    return validate_image(plan.operation(), plan.image(), device->romsize);
}

Result<FlashPlan> build_subaru_denso_sh705x_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                       std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                       KernelImage kernel)
{
    // See the note in validate_subaru_denso_sh705x_kline_plan above: this
    // function is likewise outside the anonymous namespace.
    using enum ErrorKind;
    Result<const Variant *> variant = find_variant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status valid = validate_operation(**variant, operation); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (Status valid = validate_kernel(**variant, kernel); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const flashdev_t *device = find_flash_device(mcu_type);
    if (device == nullptr)
    {
        return fail(InvalidConfig, "Unknown MCU type");
    }
    if (Status valid = validate_geometry(*device); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (Status valid = validate_image(operation, image, device->romsize); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh705xKline,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = MemoryRegion{0, device->romsize},
        .erase_regions = {},
        .image = operation == FlashOperation::Read ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan =
            SubaruDensoSh705xKlinePlan{
                .initial_baud = 4800, .tester_id = 0xF0, .target_id = 0x10, .seed_key = (*variant)->seed_key},
        .confirmations = {},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_kline_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
