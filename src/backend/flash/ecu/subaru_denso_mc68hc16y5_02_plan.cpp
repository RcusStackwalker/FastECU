#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"

#include <format>
#include <utility>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

// mainwindow.cpp:1250-1258: sub_ecu_denso_mc68hc16y5_02(_ecutek)? and the
// reachable-but-quirky _02_tpu (see spec) all construct this class; _04/
// _04_ecutek are wired to it too (mainwindow.cpp:1255-1258) but
// protocols.cfg declares read=n/a, test_write=no, write=no for both --
// rejected outright, not merely un-writable.
Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    using enum ErrorKind;
    if (protocol != "sub_ecu_denso_mc68hc16y5_02" &&
        protocol != "sub_ecu_denso_mc68hc16y5_02_ecutek" &&
        protocol != "sub_ecu_denso_mc68hc16y5_02_tpu" &&
        protocol != "sub_ecu_denso_mc68hc16y5_04" &&
        protocol != "sub_ecu_denso_mc68hc16y5_04_ecutek")
    {
        return fail(InvalidConfig, std::format("Unsupported MC68HC16Y5_02 protocol: {}", protocol));
    }
    if (protocol == "sub_ecu_denso_mc68hc16y5_04" || protocol == "sub_ecu_denso_mc68hc16y5_04_ecutek")
    {
        return fail(Unsupported,
                    "protocols.cfg declares no supported operation for MC68HC16Y5 revision 04");
    }
    if (find_flash_device_index(mcu) < 0)
    {
        return fail(InvalidConfig, std::format("Unknown MCU type: {}", mcu));
    }
    return {};
}

Status validate_operation(std::string_view protocol, FlashOperation operation)
{
    if (protocol == "sub_ecu_denso_mc68hc16y5_02_tpu" && operation != FlashOperation::Read)
    {
        return fail(ErrorKind::Unsupported,
                    "protocols.cfg declares no supported write or test_write operation for the MC68HC16Y5 TPU variant");
    }
    return {};
}

SubaruDensoMc68hc16y5_02Plan wire_params(std::string_view protocol)
{
    // flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:126-137 (response
    // selection), 204-242 (baud/encryption/magic selection). Only "_ecutek"
    // is reachable via protocols.cfg (see spec's "_cobb" note); every other
    // accepted name takes the stock branch.
    if (protocol.ends_with("_ecutek"))
    {
        return {.connect_baud = 9600,
                .kernel_baud = 11700,
                .encryption_xor = 0x51,
                .kernel_magic = 0x3940,
                .bootloader_ok = {0x4C, 0x00, 0xB4}};
    }
    return {.connect_baud = 9600,
            .kernel_baud = 9600,
            .encryption_xor = 0x55,
            .kernel_magic = 0x3941,
            .bootloader_ok = {0x4D, 0x00, 0xB3}};
}

} // namespace

Status validate_subaru_denso_mc68hc16y5_02_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruDensoMc68hc16y5_02 || plan.transport() != TransportKind::Kline)
    {
        return fail(InvalidConfig, "plan is not for MC68HC16Y5_02");
    }
    if (!std::holds_alternative<SubaruDensoMc68hc16y5_02Plan>(plan.family_plan()))
    {
        return fail(InvalidConfig, "MC68HC16Y5_02 wire parameters are missing");
    }
    if (!plan.kernel().has_value())
    {
        return fail(InvalidConfig, "MC68HC16Y5_02 requires a kernel image");
    }
    const int index = find_flash_device_index(plan.mcu_name());
    if (index < 0)
    {
        return fail(InvalidConfig, "Unknown MCU type");
    }
    const std::uint32_t romsize = flashdevices[index].romsize;
    if (auto valid = validate_operation(plan.target_id(), plan.operation()); !valid.has_value())
    {
        return valid;
    }
    if ((plan.operation() == FlashOperation::Write || plan.operation() == FlashOperation::TestWrite) &&
        (!plan.image().has_value() || plan.image()->size() != romsize))
    {
        return fail(InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }
    return {};
}

Result<FlashPlan> build_subaru_denso_mc68hc16y5_02_plan(FlashOperation operation,
                                                        std::string_view protocol_name,
                                                        std::string_view mcu_type,
                                                        std::optional<bytes::Bytes> image,
                                                        KernelImage kernel)
{
    if (auto valid = validate_identity(protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (auto valid = validate_operation(protocol_name, operation); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const int index = find_flash_device_index(mcu_type);
    const std::uint32_t romsize = flashdevices[index].romsize;
    if ((operation == FlashOperation::Write || operation == FlashOperation::TestWrite) &&
        (!image.has_value() || image->size() != romsize))
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("ROM file must be exactly 0x{:x} bytes", romsize));
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruDensoMc68hc16y5_02,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = MemoryRegion{flashdevices[index].fblocks[0].start, romsize},
        .erase_regions = {}, // per-block erase happens inside the write executor
                             // (blank-page-per-modified-block, legacy
                             // flash_block():950-992), not a fixed up-front set
        .image = operation == FlashOperation::Read ? std::nullopt : std::move(image),
        .kernel = std::move(kernel),
        .family_plan = wire_params(protocol_name),
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_denso_mc68hc16y5_02_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
