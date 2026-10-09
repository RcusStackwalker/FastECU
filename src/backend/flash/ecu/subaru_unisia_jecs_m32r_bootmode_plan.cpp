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

// Built-in catalog: both are read=yes, test_write=no, write=yes; Read is served
// by the 6c-3 K-Line family.
constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
    {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
});
constexpr std::uint32_t kChunk = 0x80; // upload_kernel() :312-315, write_mem() :473
// execute() :55-60 (tester_id and target_id set on :59-60)
constexpr SubaruUnisiaJecsM32rBootModeKernelPlan kKernelWire{
    .initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10};
// write_mem() :362-364.
constexpr SubaruUnisiaJecsM32rBootModeProgramPlan kProgramWire{
    .initial_baud = 19200, .tester_id = 0xf0, .target_id = 0x10};

Result<Variant> FindVariant(std::string_view protocol, std::string_view mcu)
{
    const auto variant = std::ranges::find(kVariants, protocol, &Variant::protocol);
    if (variant == kVariants.end() || variant->mcu != mcu)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Unisia Jecs M32R bootmode protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const FlashDevice *device = FindFlashDevice(mcu);
    if (device == nullptr || device->romsize != variant->rom_size)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs M32R bootmode memory map is invalid");
    }
    return *variant;
}

Status CheckOperation(FlashOperation operation)
{
    if (operation != FlashOperation::kWrite)
    {
        return Fail(ErrorKind::kUnsupported, "Unisia Jecs M32R bootmode supports only Write here");
    }
    return {};
}

std::vector<ConfirmationSpec> VoltageConfirmation()
{
    return {ConfirmationSpec{ConfirmationSpec::Id::kApplyBootModeVoltages, {}}};
}

bool HasOnlyVoltageConfirmation(const FlashPlan& plan)
{
    const auto& confirmations = plan.Confirmations();
    return confirmations.size() == 1 && confirmations[0].id == ConfirmationSpec::Id::kApplyBootModeVoltages;
}

template <typename Wire> bool WireMatches(const Wire& wire, const Wire& expected)
{
    return wire.initial_baud == expected.initial_baud && wire.tester_id == expected.tester_id &&
           wire.target_id == expected.target_id;
}

Status ValidateKernel(const FlashPlan& plan)
{
    const auto *wire = std::get_if<SubaruUnisiaJecsM32rBootModeKernelPlan>(&plan.FamilyPlan());
    if (wire == nullptr || !WireMatches(*wire, kKernelWire))
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs M32R bootmode kernel wire parameters are invalid");
    }
    if (!plan.Image().has_value() || plan.Image()->empty() || plan.Image()->size() % kChunk != 0 ||
        plan.TransferRegion() != MemoryRegion{0, static_cast<std::uint32_t>(plan.Image()->size())})
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs M32R bootmode kernel image shape is invalid");
    }
    return {};
}

Status ValidateProgram(const FlashPlan& plan, const Variant& variant)
{
    const auto *wire = std::get_if<SubaruUnisiaJecsM32rBootModeProgramPlan>(&plan.FamilyPlan());
    if (wire == nullptr || !WireMatches(*wire, kProgramWire))
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs M32R bootmode program wire parameters are invalid");
    }
    if (!plan.Image().has_value() || plan.Image()->size() != variant.rom_size ||
        plan.TransferRegion() != MemoryRegion{0, variant.rom_size})
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs M32R bootmode write image must be exactly the ROM size");
    }
    return {};
}

Result<FlashPlan> Build(FlashFamily family, const Variant& variant, bytes::Bytes image, FamilyPlan wire)
{
    const auto length = static_cast<std::uint32_t>(image.size());
    auto plan = ValidateAndBuild(FlashPlanFields{
        .operation = FlashOperation::kWrite,
        .family = family,
        .transport = TransportKind::kKline,
        .target_id = std::string(variant.protocol),
        .mcu_name = std::string(variant.mcu),
        .transfer_region = {0, length},
        .erase_regions = {},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = std::move(wire),
        .confirmations = VoltageConfirmation(),
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = ValidateSubaruUnisiaJecsM32rBootmodePlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace

Status ValidateSubaruUnisiaJecsM32rBootmodePlan(const FlashPlan& plan)
{
    const bool kernel = plan.Family() == FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel;
    if ((!kernel && plan.Family() != FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram) ||
        plan.Transport() != TransportKind::kKline)
    {
        return Fail(ErrorKind::kInvalidConfig, "plan is not for Subaru Unisia Jecs M32R bootmode");
    }
    const auto variant = FindVariant(plan.TargetId(), plan.McuName());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status operation = CheckOperation(plan.Operation()); !operation.has_value())
    {
        return operation;
    }
    if (!plan.EraseRegions().empty() || plan.Kernel().has_value() || !HasOnlyVoltageConfirmation(plan))
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs M32R bootmode plan shape is invalid");
    }
    return kernel ? ValidateKernel(plan) : ValidateProgram(plan, *variant);
}

Result<FlashPlan> BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(FlashOperation operation, std::string_view protocol_name,
                                                              std::string_view mcu_type, bytes::Bytes kernel)
{
    const auto variant = FindVariant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status checked = CheckOperation(operation); !checked.has_value())
    {
        return std::unexpected(checked.error());
    }
    if (kernel.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs M32R bootmode kernel file is empty");
    }
    // upload_kernel() :312-315: zero-pad to whole 128-byte chunks.
    kernel.resize((kernel.size() + kChunk - 1) / kChunk * kChunk, 0x00);
    return Build(FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel, *variant, std::move(kernel), kKernelWire);
}

Result<FlashPlan> BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(FlashOperation operation, std::string_view protocol_name,
                                                               std::string_view mcu_type,
                                                               std::optional<bytes::Bytes> image)
{
    const auto variant = FindVariant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status checked = CheckOperation(operation); !checked.has_value())
    {
        return std::unexpected(checked.error());
    }
    if (!image.has_value() || image->size() != variant->rom_size)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} write image must be exactly {} bytes, not {}", variant->protocol, variant->rom_size,
                                image.has_value() ? image->size() : 0));
    }
    return Build(FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram, *variant, std::move(*image), kProgramWire);
}
} // namespace fastecu::flash
