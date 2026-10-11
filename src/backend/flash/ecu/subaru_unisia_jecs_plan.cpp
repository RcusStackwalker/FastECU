#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
struct Identity
{
    std::string_view protocol;
    std::string_view mcu;
};

constexpr auto kIdentities = std::to_array<Identity>({
    {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
    {"sub_ecu_unisia_jecs_m3775x", "M3775x"},
});
constexpr MemoryRegion kRom{0, 0x10000};
constexpr int kInitialBaud = 1953;

Status ValidateIdentity(std::string_view protocol, std::string_view mcu)
{
    const auto identity = std::ranges::find(kIdentities, protocol, &Identity::protocol);
    if (identity == kIdentities.end() || identity->mcu != mcu)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Unisia Jecs protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const FlashDevice *device = FindFlashDevice(mcu);
    if (device == nullptr || device->romsize != kRom.length)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs flash geometry is invalid");
    }
    return {};
}
} // namespace

Status ValidateSubaruUnisiaJecsPlan(const FlashPlan& plan)
{
    if (plan.Family() != FlashFamily::kSubaruUnisiaJecs || plan.Transport() != TransportKind::kKline)
    {
        return Fail(ErrorKind::kInvalidConfig, "plan is not for Subaru Unisia Jecs");
    }
    if (auto identity = ValidateIdentity(plan.TargetId(), plan.McuName()); !identity.has_value())
    {
        return identity;
    }
    if (plan.Operation() != FlashOperation::kRead)
    {
        return Fail(ErrorKind::kUnsupported, "Unisia Jecs is read-only");
    }
    const auto *wire = std::get_if<SubaruUnisiaJecsPlan>(&plan.FamilyPlan());
    if (wire == nullptr || wire->initial_baud != kInitialBaud || !wire->even_parity)
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs wire parameters are invalid");
    }
    if (plan.TransferRegion().start != kRom.start || plan.TransferRegion().length != kRom.length ||
        !plan.EraseRegions().empty() || plan.Image().has_value() || plan.Kernel().has_value() ||
        !plan.Confirmations().empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs read-plan shape is invalid");
    }
    return {};
}

Result<FlashPlan> BuildSubaruUnisiaJecsPlan(FlashOperation operation, std::string_view protocol_name,
                                            std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    if (auto identity = ValidateIdentity(protocol_name, mcu_type); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    if (operation != FlashOperation::kRead)
    {
        return Fail(ErrorKind::kUnsupported, "Unisia Jecs is read-only");
    }
    if (image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "Unisia Jecs read plans must not carry an image");
    }
    auto plan = ValidateAndBuild(FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::kSubaruUnisiaJecs,
        .transport = TransportKind::kKline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = kRom,
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruUnisiaJecsPlan{.initial_baud = kInitialBaud, .even_parity = true},
        .confirmations = {},
    });
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = ValidateSubaruUnisiaJecsPlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
