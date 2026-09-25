#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
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
    bool writable;
};

// protocols.cfg: _20 and _30 are read/write; _40 and _70 are read-only.
// test_write is "no" for all four. The two _bootmode names are Read-only
// here: their Read is this family's wire sequence (wave 7), and their Write
// is the bootmode family's two-attempt kernel upload and program.
constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000, true},
    {"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000, true},
    {"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000, false},
    {"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000, false},
    {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000, false},
    {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000, false},
});
// Legacy read_mem() :219 reads fblocks[0].start + 0x100000, and every M32R
// variant above starts at 0; write_mem() :522 programs from flash address 0.
constexpr std::uint32_t kReadBase = 0x100000;
// Legacy execute() :55-57.
constexpr SubaruUnisiaJecsM32rKlinePlan kWire{.initial_baud = 4800, .tester_id = 0xf0, .target_id = 0x10};

Result<Variant> find_variant(std::string_view protocol, std::string_view mcu)
{
    const auto variant = std::ranges::find(kVariants, protocol, &Variant::protocol);
    if (variant == kVariants.end() || variant->mcu != mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Unisia Jecs M32R protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const flashdev_t *device = find_flash_device(mcu);
    if (device == nullptr || device->romsize != variant->rom_size || device->fblocks == nullptr ||
        device->fblocks[0].start != 0)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R memory map is invalid");
    }
    return *variant;
}

Status check_operation(const Variant& variant, FlashOperation operation)
{
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "Unisia Jecs M32R has no test write");
    }
    if (operation == FlashOperation::Write && !variant.writable)
    {
        return fail(ErrorKind::Unsupported, std::format("{} is read-only", variant.protocol));
    }
    return {};
}

MemoryRegion region_for(const Variant& variant, FlashOperation operation)
{
    return operation == FlashOperation::Read ? MemoryRegion{kReadBase, variant.rom_size}
                                             : MemoryRegion{0, variant.rom_size};
}
} // namespace

Status validate_subaru_unisia_jecs_m32r_kline_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruUnisiaJecsM32rKline || plan.transport() != TransportKind::Kline)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Unisia Jecs M32R");
    }
    const auto variant = find_variant(plan.target_id(), plan.mcu_name());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status operation = check_operation(*variant, plan.operation()); !operation.has_value())
    {
        return operation;
    }
    const auto *wire = std::get_if<SubaruUnisiaJecsM32rKlinePlan>(&plan.family_plan());
    if (wire == nullptr || wire->initial_baud != kWire.initial_baud || wire->tester_id != kWire.tester_id ||
        wire->target_id != kWire.target_id)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R wire parameters are invalid");
    }
    if (!plan.erase_regions().empty() || plan.kernel().has_value() ||
        plan.transfer_region() != region_for(*variant, plan.operation()))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R plan shape is invalid");
    }
    const auto& confirmations = plan.confirmations();
    if (plan.operation() == FlashOperation::Read)
    {
        if (plan.image().has_value() || !confirmations.empty())
        {
            return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R read-plan shape is invalid");
        }
        return {};
    }
    if (!plan.image().has_value() || plan.image()->size() != variant->rom_size)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R write image must be exactly the ROM size");
    }
    if (confirmations.size() > 1 ||
        (confirmations.size() == 1 && confirmations[0].id != ConfirmationSpec::Id::ApplyProgrammingVoltage))
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R write confirmations are invalid");
    }
    return {};
}

Result<FlashPlan> build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type, std::optional<bytes::Bytes> image,
                                                           bool adapter_supplies_programming_voltage)
{
    const auto variant = find_variant(protocol_name, mcu_type);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (Status checked = check_operation(*variant, operation); !checked.has_value())
    {
        return std::unexpected(checked.error());
    }
    if (operation == FlashOperation::Read)
    {
        if (image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "Unisia Jecs M32R read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != variant->rom_size)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} write image must be exactly {} bytes, not {}", variant->protocol, variant->rom_size,
                                image.has_value() ? image->size() : 0));
    }

    std::vector<ConfirmationSpec> confirmations;
    if (operation == FlashOperation::Write && !adapter_supplies_programming_voltage)
    {
        confirmations.push_back(ConfirmationSpec{ConfirmationSpec::Id::ApplyProgrammingVoltage, {}});
    }

    auto plan = validate_and_build(FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::SubaruUnisiaJecsM32rKline,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = region_for(*variant, operation),
        .erase_regions = {},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = kWire,
        .confirmations = std::move(confirmations),
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_unisia_jecs_m32r_kline_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
