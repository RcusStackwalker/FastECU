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
Status ValidateIdentity(const SingleWindowPlanSpec& spec, std::string_view protocol, std::string_view mcu)
{
    if (std::ranges::find(spec.protocols, protocol) == spec.protocols.end())
    {
        return Fail(kInvalidConfig, std::format("Unsupported {} protocol: {}", spec.display_name, protocol));
    }
    const int index = FindFlashDeviceIndex(mcu);
    if (index < 0)
    {
        return Fail(kInvalidConfig, std::format("Unknown MCU type: {}", mcu));
    }
    if (mcu != spec.mcu)
    {
        return Fail(kInvalidConfig, std::format("Protocol {} expects MCU {}; got {}", protocol, spec.mcu, mcu));
    }
    if (!spec.geometry_ok(kFlashDevices[index]))
    {
        return Fail(kInvalidConfig, std::format("{} flash geometry is invalid", spec.mcu));
    }
    return {};
}
} // namespace

Status ValidateSingleWindowPlan(const SingleWindowPlanSpec& spec, const FlashPlan& plan)
{
    if (auto valid = ValidateIdentity(spec, plan.TargetId(), plan.McuName()); !valid.has_value())
    {
        return valid;
    }
    if (plan.Family() != spec.family || plan.Transport() != spec.transport)
    {
        return Fail(kInvalidConfig, std::format("plan is not for {}", spec.display_name));
    }
    if (!spec.wire_params_ok(plan))
    {
        return Fail(kInvalidConfig, std::format("{} wire parameters are invalid", spec.display_name));
    }
    if (const MemoryRegion& expected = plan.Operation() == FlashOperation::kRead ? spec.read_region : spec.write_region;
        plan.TransferRegion().start != expected.start || plan.TransferRegion().length != expected.length)
    {
        return Fail(kInvalidConfig, std::format("{} transfer region is invalid", spec.display_name));
    }
    if (plan.Kernel())
    {
        return Fail(kInvalidConfig, std::format("{} plans are kernel-free", spec.display_name));
    }
    if (plan.Operation() == FlashOperation::kTestWrite)
    {
        return Fail(kUnsupported, "test_write is not supported by this family");
    }
    if (!spec.supports_write && plan.Operation() == FlashOperation::kWrite)
    {
        return Fail(kUnsupported, std::format("write is not supported by {}", spec.display_name));
    }
    if (plan.Operation() == FlashOperation::kRead && !plan.EraseRegions().empty())
    {
        return Fail(kInvalidConfig, "read plans must not erase memory");
    }
    if (plan.Operation() == FlashOperation::kWrite &&
        (plan.EraseRegions().size() != 1 || plan.EraseRegions()[0].start != spec.write_region.start ||
         plan.EraseRegions()[0].length != spec.write_region.length))
    {
        return Fail(kInvalidConfig, std::format("{} erase region is invalid", spec.display_name));
    }
    if (plan.Operation() == FlashOperation::kWrite &&
        (!plan.Image().has_value() || plan.Image()->size() != spec.image_size))
    {
        return Fail(kInvalidConfig, std::format("ROM file must be exactly 0x{:X} bytes", spec.image_size));
    }
    return {};
}

Result<FlashPlan> BuildSingleWindowPlan(const SingleWindowPlanSpec& spec, FlashOperation operation,
                                        std::string_view protocol_name, std::string_view mcu_type,
                                        std::optional<bytes::Bytes> image, FamilyPlan family_plan)
{
    if (auto valid = ValidateIdentity(spec, protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (operation == FlashOperation::kTestWrite)
    {
        return Fail(kUnsupported, "test_write is not supported by this family");
    }
    if (!spec.supports_write && operation == FlashOperation::kWrite)
    {
        return Fail(kUnsupported, std::format("write is not supported by {}", spec.display_name));
    }
    if (operation == FlashOperation::kWrite)
    {
        if (!image.has_value())
        {
            return Fail(kInvalidConfig, "Write plans must carry a ROM image");
        }
        if (image->size() != spec.image_size)
        {
            return Fail(kInvalidConfig, std::format("ROM file must be exactly 0x{:X} bytes; got 0x{:x} bytes",
                                                    spec.image_size, image->size()));
        }
    }
    FlashPlanFields fields{
        .operation = operation,
        .family = spec.family,
        .transport = spec.transport,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = operation == FlashOperation::kRead ? spec.read_region : spec.write_region,
        .erase_regions =
            operation == FlashOperation::kWrite ? std::vector{spec.write_region} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::kWrite ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = std::move(family_plan),
    };
    auto plan = ValidateAndBuild(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = ValidateSingleWindowPlan(spec, *plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
