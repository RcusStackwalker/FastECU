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

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    const auto identity = std::ranges::find(kIdentities, protocol, &Identity::protocol);
    if (identity == kIdentities.end() || identity->mcu != mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Unisia Jecs protocol '{}' does not match MCU '{}'", protocol, mcu));
    }
    const flashdev_t *device = find_flash_device(mcu);
    if (device == nullptr || device->romsize != kRom.length)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs flash geometry is invalid");
    }
    return {};
}
} // namespace

Status validate_subaru_unisia_jecs_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruUnisiaJecs || plan.transport() != TransportKind::Kline)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Unisia Jecs");
    }
    if (auto identity = validate_identity(plan.target_id(), plan.mcu_name()); !identity.has_value())
    {
        return identity;
    }
    if (plan.operation() != FlashOperation::Read)
    {
        return fail(ErrorKind::Unsupported, "Unisia Jecs is read-only");
    }
    const auto *wire = std::get_if<SubaruUnisiaJecsPlan>(&plan.family_plan());
    if (wire == nullptr || wire->initial_baud != kInitialBaud || !wire->even_parity)
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs wire parameters are invalid");
    }
    if (plan.transfer_region().start != kRom.start || plan.transfer_region().length != kRom.length ||
        !plan.erase_regions().empty() || plan.image().has_value() || plan.kernel().has_value() ||
        !plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs read-plan shape is invalid");
    }
    return {};
}

Result<FlashPlan> build_subaru_unisia_jecs_plan(FlashOperation operation, std::string_view protocol_name,
                                                std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    if (auto identity = validate_identity(protocol_name, mcu_type); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    if (operation != FlashOperation::Read)
    {
        return fail(ErrorKind::Unsupported, "Unisia Jecs is read-only");
    }
    if (image.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Unisia Jecs read plans must not carry an image");
    }
    auto plan = validate_and_build(FlashPlanFields{
        .operation = operation,
        .family = FlashFamily::SubaruUnisiaJecs,
        .transport = TransportKind::Kline,
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
    if (auto valid = validate_subaru_unisia_jecs_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
