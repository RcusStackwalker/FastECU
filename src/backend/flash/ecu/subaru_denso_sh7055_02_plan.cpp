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
    if (!std::holds_alternative<SubaruDensoSh7055_02Plan>(plan.family_plan()))
    {
        return fail(InvalidConfig, "SH7055_02 wire parameters are missing");
    }
    if (!plan.kernel().has_value())
    {
        return fail(InvalidConfig, "SH7055_02 requires a kernel image");
    }
    const int index = find_flash_device_index(plan.mcu_name());
    if (index < 0)
    {
        return fail(InvalidConfig, "Unknown MCU type");
    }
    return validate_image(plan, flashdevices[index].romsize);
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
