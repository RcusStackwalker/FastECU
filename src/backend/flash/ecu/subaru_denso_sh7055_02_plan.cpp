#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"

#include <format>
#include <utility>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    using enum ErrorKind;
    if (protocol != "sub_ecu_denso_sh7055_02" && protocol != "sub_ecu_denso_sh7055_02_ecutek")
    {
        return fail(InvalidConfig, std::format("Unsupported SH7055_02 protocol: {}", protocol));
    }
    if (find_flash_device_index(mcu) < 0)
    {
        return fail(InvalidConfig, std::format("Unknown MCU type: {}", mcu));
    }
    return {};
}

SubaruDensoSh7055_02Plan wire_params(FlashOperation operation)
{
    // connect_bootloader():133-168 gates the SSM ECU-ID request on cmd_type
    // being "read". The selected suffix never changes these parameters.
    return {.tester_id = 0xf0, .target_id = 0x10, .read_ecu_id = operation == FlashOperation::Read};
}

Status validate_image(const FlashPlan& plan, std::uint32_t romsize)
{
    if ((plan.operation() == FlashOperation::Write || plan.operation() == FlashOperation::TestWrite) &&
        (!plan.image().has_value() || plan.image()->size() != romsize))
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }
    return {};
}

Status validate_kernel_upload(const KernelImage& kernel)
{
    constexpr std::uint64_t kMaxWireLength = 0x00FFFFFF;
    if (kernel.bytes.size() > kMaxWireLength)
    {
        return fail(ErrorKind::InvalidConfig,
                    "SH7055_02 padded kernel plus envelope exceeds the 24-bit wire length");
    }
    const std::uint64_t padded_size = (static_cast<std::uint64_t>(kernel.bytes.size()) + 3) & ~3ULL;
    const std::uint64_t wire_length = padded_size + 4;
    if (wire_length > kMaxWireLength)
    {
        return fail(ErrorKind::InvalidConfig,
                    "SH7055_02 padded kernel plus envelope exceeds the 24-bit wire length");
    }
    // Model source: src/backend/definitions/kernelmemorymodels.h:270-276.
    constexpr std::uint64_t kKernelStart = 0xFFFF6004;
    constexpr std::uint64_t kKernelLength = 0x00006000;
    constexpr std::uint64_t kKernelEnd = kKernelStart + kKernelLength;
    const std::uint64_t upload_start = kernel.load_address;
    if (upload_start < kKernelStart || upload_start >= kKernelEnd ||
        padded_size > kKernelEnd - upload_start)
    {
        return fail(ErrorKind::InvalidConfig,
                    "SH7055_02 padded kernel upload is outside the model kernel region");
    }
    return {};
}

} // namespace

Status validate_subaru_denso_sh7055_02_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruDensoSh7055_02 || plan.transport() != TransportKind::Kline)
    {
        return fail(InvalidConfig, "plan is not for SH7055_02");
    }
    const auto *family = std::get_if<SubaruDensoSh7055_02Plan>(&plan.family_plan());
    if (family == nullptr)
    {
        return fail(InvalidConfig, "SH7055_02 wire parameters are missing");
    }
    if (family->tester_id != 0xf0 || family->target_id != 0x10)
    {
        return fail(InvalidConfig, "SH7055_02 wire parameters are invalid");
    }
    if (family->read_ecu_id != (plan.operation() == FlashOperation::Read))
    {
        return fail(InvalidConfig, "SH7055_02 ECU-ID read does not match the operation");
    }
    if (!plan.erase_regions().empty())
    {
        return fail(InvalidConfig, "SH7055_02 plans must not declare erase regions");
    }
    if (!plan.kernel().has_value())
    {
        return fail(InvalidConfig, "SH7055_02 requires a kernel image");
    }
    if (auto valid = validate_kernel_upload(*plan.kernel()); !valid.has_value())
    {
        return valid;
    }
    if (plan.confirmations().size() != 1 ||
        plan.confirmations().front().id != ConfirmationSpec::Id::CycleIgnition ||
        !plan.confirmations().front().arguments.empty())
    {
        return fail(InvalidConfig, "SH7055_02 requires exactly the CycleIgnition confirmation");
    }
    const int index = find_flash_device_index(plan.mcu_name());
    if (index < 0)
    {
        return fail(InvalidConfig, "Unknown MCU type");
    }
    const std::uint32_t romsize = flashdevices[index].romsize;
    if (plan.transfer_region().start != flashdevices[index].fblocks[0].start ||
        plan.transfer_region().length != romsize)
    {
        return fail(InvalidConfig, "SH7055_02 transfer region does not match the MCU");
    }
    return validate_image(plan, romsize);
}

Result<FlashPlan> build_subaru_denso_sh7055_02_plan(FlashOperation operation,
                                                    std::string_view protocol_name,
                                                    std::string_view mcu_type,
                                                    std::optional<bytes::Bytes> image,
                                                    KernelImage kernel)
{
    if (auto valid = validate_identity(protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const int index = find_flash_device_index(mcu_type);
    const std::uint32_t romsize = flashdevices[index].romsize;
    if ((operation == FlashOperation::Write || operation == FlashOperation::TestWrite) &&
        (!image.has_value() || image->size() != romsize))
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }
    if (auto valid = validate_kernel_upload(kernel); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh7055_02,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = MemoryRegion{flashdevices[index].fblocks[0].start, romsize},
        .erase_regions = {},
        .image = operation == FlashOperation::Read ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = wire_params(operation),
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_denso_sh7055_02_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
