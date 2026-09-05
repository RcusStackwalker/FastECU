#include "src/backend/flash/ecu/single_window_plan.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
using enum ErrorKind;

// Identity is checked in the same order every family checked it before the
// extraction: protocol name, then that the MCU is known at all, then that it
// is this family's MCU, then the family's own flash-table geometry.
Status validate_identity(const SingleWindowPlanSpec& spec, std::string_view protocol, std::string_view mcu)
{
    if (std::ranges::find(spec.protocols, protocol) == spec.protocols.end())
    {
        return fail(InvalidConfig, std::format("Unsupported {} protocol: {}", spec.display_name, protocol));
    }
    const int index = find_flash_device_index(mcu);
    if (index < 0)
    {
        return fail(InvalidConfig, std::format("Unknown MCU type: {}", mcu));
    }
    if (mcu != spec.mcu)
    {
        return fail(InvalidConfig, std::format("Protocol {} expects MCU {}; got {}", protocol, spec.mcu, mcu));
    }
    if (!spec.geometry_ok(flashdevices[index]))
    {
        return fail(InvalidConfig, std::format("{} flash geometry is invalid", spec.mcu));
    }
    return {};
}
} // namespace

Status validate_single_window_plan(const SingleWindowPlanSpec& spec, const FlashPlan& plan)
{
    if (auto valid = validate_identity(spec, plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != spec.family || plan.transport() != spec.transport)
    {
        return fail(InvalidConfig, std::format("plan is not for {}", spec.display_name));
    }
    if (!spec.wire_params_ok(plan))
    {
        return fail(InvalidConfig, std::format("{} wire parameters are invalid", spec.display_name));
    }
    if (const MemoryRegion& expected = plan.operation() == FlashOperation::Read ? spec.read_region : spec.write_region;
        plan.transfer_region().start != expected.start || plan.transfer_region().length != expected.length)
    {
        return fail(InvalidConfig, std::format("{} transfer region is invalid", spec.display_name));
    }
    if (plan.kernel())
    {
        return fail(InvalidConfig, std::format("{} plans are kernel-free", spec.display_name));
    }
    if (plan.operation() == FlashOperation::TestWrite)
    {
        return fail(Unsupported, "test_write is not supported by this family");
    }
    if (plan.operation() == FlashOperation::Read && !plan.erase_regions().empty())
    {
        return fail(InvalidConfig, "read plans must not erase memory");
    }
    if (plan.operation() == FlashOperation::Write &&
        (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != spec.write_region.start ||
         plan.erase_regions()[0].length != spec.write_region.length))
    {
        return fail(InvalidConfig, std::format("{} erase region is invalid", spec.display_name));
    }
    if (plan.operation() == FlashOperation::Write &&
        (!plan.image().has_value() || plan.image()->size() != spec.image_size))
    {
        return fail(InvalidConfig, std::format("ROM file must be exactly 0x{:X} bytes", spec.image_size));
    }
    return {};
}

Result<FlashPlan> build_single_window_plan(const SingleWindowPlanSpec& spec, FlashOperation operation,
                                           std::string_view protocol_name, std::string_view mcu_type,
                                           std::optional<bytes::Bytes> image, FamilyPlan family_plan)
{
    if (auto valid = validate_identity(spec, protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (operation == FlashOperation::TestWrite)
    {
        return fail(Unsupported, "test_write is not supported by this family");
    }
    if (operation == FlashOperation::Write)
    {
        if (!image.has_value())
        {
            return fail(InvalidConfig, "Write plans must carry a ROM image");
        }
        if (image->size() != spec.image_size)
        {
            return fail(InvalidConfig, std::format("ROM file must be exactly 0x{:X} bytes; got 0x{:x} bytes",
                                                   spec.image_size, image->size()));
        }
    }
    FlashPlanFields fields{
        .operation = operation,
        .family = spec.family,
        .transport = spec.transport,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = operation == FlashOperation::Read ? spec.read_region : spec.write_region,
        .erase_regions =
            operation == FlashOperation::Write ? std::vector{spec.write_region} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = std::move(family_plan),
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_single_window_plan(spec, *plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
