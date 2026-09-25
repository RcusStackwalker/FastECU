#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
struct Variant
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
};

// protocols.cfg: both are read=yes, test_write=no, write=yes; Read is served
// by the 6c-3 K-Line family.
constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
    {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
});
constexpr std::uint32_t kChunk = 0x80; // upload_kernel() :312-315, write_mem() :473
// execute() :55-60 (tester_id and target_id set on :59-60)
constexpr SubaruUnisiaJecsM32rBootModeKernelPlan kKernelWire{
    .initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10};
constexpr SubaruUnisiaJecsM32rBootModeProgramPlan kProgramWire{
    .initial_baud = 19200, .tester_id = 0xf0, .target_id = 0x10};

Result<Variant> find_variant(std::string_view protocol, std::string_view mcu)
{
    const auto variant = std::ranges::find(kVariants, protocol, &Variant::protocol);
    if (variant == kVariants.end() || variant->mcu != mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Unisia Jecs M32R bootmode protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const flashdev_t *device = find_flash_device(mcu);
    if (device == nullptr || device->romsize != variant->rom_size)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode memory map is invalid");
    }
    return *variant;
}

Status check_operation(FlashOperation operation)
{
    if (operation != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported, "Unisia Jecs M32R bootmode supports only Write here");
    }
    return {};
}

std::vector<ConfirmationSpec> voltage_confirmation()
{
    return {ConfirmationSpec{ConfirmationSpec::Id::ApplyBootModeVoltages, {}}};
}

bool has_only_voltage_confirmation(const FlashPlan& plan)
{
    const auto& confirmations = plan.confirmations();
    return confirmations.size() == 1 && confirmations[0].id == ConfirmationSpec::Id::ApplyBootModeVoltages;
}

template <typename Wire> bool wire_matches(const Wire& wire, const Wire& expected)
{
    return wire.initial_baud == expected.initial_baud && wire.tester_id == expected.tester_id &&
           wire.target_id == expected.target_id;
}

Status validate_kernel(const FlashPlan& plan)
{
    const auto *wire = std::get_if<SubaruUnisiaJecsM32rBootModeKernelPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_matches(*wire, kKernelWire))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode kernel wire parameters are invalid");
    }
    if (!plan.image().has_value() || plan.image()->empty() || plan.image()->size() % kChunk != 0 ||
        plan.transfer_region() != MemoryRegion{0, static_cast<std::uint32_t>(plan.image()->size())})
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode kernel image shape is invalid");
    }
    return {};
}

Status validate_program(const FlashPlan& plan, const Variant& variant)
{
    const auto *wire = std::get_if<SubaruUnisiaJecsM32rBootModeProgramPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_matches(*wire, kProgramWire))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode program wire parameters are invalid");
    }
    if (!plan.image().has_value() || plan.image()->size() != variant.rom_size ||
        plan.transfer_region() != MemoryRegion{0, variant.rom_size})
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode write image must be exactly the ROM size");
    }
    return {};
}

Result<FlashPlan> build(FlashFamily family, const Variant& variant, bytes::Bytes image, FamilyPlan wire)
{
    const auto length = static_cast<std::uint32_t>(image.size());
    auto plan = validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = family,
        .transport = TransportKind::Kline,
        .target_id = std::string(variant.protocol),
        .mcu_name = std::string(variant.mcu),
        .transfer_region = {0, length},
        .erase_regions = {},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = std::move(wire),
        .confirmations = voltage_confirmation(),
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace

Status validate_subaru_unisia_jecs_m32r_bootmode_plan(const FlashPlan& plan)
{
    const bool kernel = plan.family() == FlashFamily::SubaruUnisiaJecsM32rBootModeKernel;
    if ((!kernel && plan.family() != FlashFamily::SubaruUnisiaJecsM32rBootModeProgram) ||
        plan.transport() != TransportKind::Kline)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Unisia Jecs M32R bootmode");
    }
    const auto variant = find_variant(plan.target_id(), plan.mcu_name());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status operation = check_operation(plan.operation()); !operation.has_value())
    {
        return operation;
    }
    if (!plan.erase_regions().empty() || plan.kernel().has_value() || !has_only_voltage_confirmation(plan))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode plan shape is invalid");
    }
    return kernel ? validate_kernel(plan) : validate_program(plan, *variant);
}

Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(FlashOperation operation,
                                                                     std::string_view protocol_name,
                                                                     std::string_view mcu_type, bytes::Bytes kernel)
{
    const auto variant = find_variant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status checked = check_operation(operation); !checked.has_value())
    {
        return std::unexpected(checked.error());
    }
    if (kernel.empty())
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R bootmode kernel file is empty");
    }
    // upload_kernel() :312-315: zero-pad to whole 128-byte chunks.
    kernel.resize((kernel.size() + kChunk - 1) / kChunk * kChunk, 0x00);
    return build(FlashFamily::SubaruUnisiaJecsM32rBootModeKernel, *variant, std::move(kernel), kKernelWire);
}

Result<FlashPlan> build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation operation,
                                                                      std::string_view protocol_name,
                                                                      std::string_view mcu_type,
                                                                      std::optional<bytes::Bytes> image)
{
    const auto variant = find_variant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status checked = check_operation(operation); !checked.has_value())
    {
        return std::unexpected(checked.error());
    }
    if (!image.has_value() || image->size() != variant->rom_size)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} write image must be exactly {} bytes, not {}", variant->protocol, variant->rom_size,
                                image.has_value() ? image->size() : 0));
    }
    return build(FlashFamily::SubaruUnisiaJecsM32rBootModeProgram, *variant, std::move(*image), kProgramWire);
}
} // namespace fastecu::flash
