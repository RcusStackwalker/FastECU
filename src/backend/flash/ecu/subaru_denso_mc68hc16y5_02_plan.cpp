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
    if (protocol != "sub_ecu_denso_mc68hc16y5_02" && protocol != "sub_ecu_denso_mc68hc16y5_02_ecutek" &&
        protocol != "sub_ecu_denso_mc68hc16y5_02_tpu" && protocol != "sub_ecu_denso_mc68hc16y5_04" &&
        protocol != "sub_ecu_denso_mc68hc16y5_04_ecutek")
    {
        return fail(InvalidConfig, std::format("Unsupported MC68HC16Y5_02 protocol: {}", protocol));
    }
    if (protocol == "sub_ecu_denso_mc68hc16y5_04" || protocol == "sub_ecu_denso_mc68hc16y5_04_ecutek")
    {
        return fail(Unsupported, "protocols.cfg declares no supported operation for MC68HC16Y5 revision 04");
    }
    if (const std::string_view expected_mcu =
            protocol == "sub_ecu_denso_mc68hc16y5_02_tpu" ? "MC68HC16Y5_TPU" : "MC68HC16Y5";
        mcu != expected_mcu)
    {
        return fail(InvalidConfig, std::format("protocol {} requires MCU {}, not {}", protocol, expected_mcu, mcu));
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

Status validate_kernel_upload(const KernelImage& kernel)
{
    // Legacy upload_kernel() serializes only address bits 23..8, so the low
    // byte is implicit zero. The catalog and shared device table both place
    // this kernel at the one canonical 0x20000 model region.
    constexpr std::uint32_t kKernelStart = 0x00020000;
    constexpr std::uint64_t kKernelLength = 0x00008000;
    constexpr std::uint64_t kMaxWireLength = 0x00ffffff;
    if (kernel.load_address != kKernelStart)
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5_02 kernel address is not the canonical wire address");
    }
    const std::uint64_t padded_size = (static_cast<std::uint64_t>(kernel.bytes.size()) + 0x0f) & ~0x0fULL;
    if (padded_size > kMaxWireLength)
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5_02 padded kernel exceeds the 24-bit wire length");
    }
    if (padded_size > kKernelLength)
    {
        return fail(ErrorKind::InvalidConfig, "MC68HC16Y5_02 padded kernel is outside the model kernel region");
    }
    return {};
}

} // namespace

Status validate_subaru_denso_mc68hc16y5_02_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (plan.family() != FlashFamily::SubaruDensoMc68hc16y5_02 || plan.transport() != TransportKind::Kline)
    {
        return fail(InvalidConfig, "plan is not for MC68HC16Y5_02");
    }
    const auto *family = std::get_if<SubaruDensoMc68hc16y5_02Plan>(&plan.family_plan());
    if (family == nullptr)
    {
        return fail(InvalidConfig, "MC68HC16Y5_02 wire parameters are missing");
    }
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (const SubaruDensoMc68hc16y5_02Plan expected = wire_params(plan.target_id());
        family->connect_baud != expected.connect_baud || family->kernel_baud != expected.kernel_baud ||
        family->encryption_xor != expected.encryption_xor || family->kernel_magic != expected.kernel_magic ||
        family->bootloader_ok != expected.bootloader_ok)
    {
        return fail(InvalidConfig, "MC68HC16Y5_02 wire parameters are invalid");
    }
    if (!plan.kernel().has_value())
    {
        return fail(InvalidConfig, "MC68HC16Y5_02 requires a kernel image");
    }
    if (auto valid = validate_kernel_upload(*plan.kernel()); !valid.has_value())
    {
        return valid;
    }
    if (!plan.erase_regions().empty())
    {
        return fail(InvalidConfig, "MC68HC16Y5_02 plans must not declare erase regions");
    }
    if (!plan.confirmations().empty())
    {
        return fail(InvalidConfig, "MC68HC16Y5_02 plans must not declare confirmations");
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
        return fail(InvalidConfig, "MC68HC16Y5_02 transfer region does not match the MCU");
    }
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

Result<FlashPlan> build_subaru_denso_mc68hc16y5_02_plan(FlashOperation operation, std::string_view protocol_name,
                                                        std::string_view mcu_type, std::optional<bytes::Bytes> image,
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
    if (auto valid = validate_kernel_upload(kernel); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const int index = find_flash_device_index(mcu_type);
    if (index < 0)
    {
        return fail(ErrorKind::InvalidConfig, "Unknown MCU type");
    }
    const std::uint32_t romsize = flashdevices[index].romsize;
    if ((operation == FlashOperation::Write || operation == FlashOperation::TestWrite) &&
        (!image.has_value() || image->size() != romsize))
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", romsize));
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
