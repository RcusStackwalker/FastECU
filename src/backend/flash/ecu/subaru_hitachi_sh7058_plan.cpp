#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"

#include <utility>

#include "src/backend/flash/ecu/subaru_hitachi_sh7058_types.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
constexpr std::string_view kProtocol = "sub_ecu_hitachi_sh7058_can";
constexpr std::string_view kMcu = "SH7058_1block";
constexpr std::uint32_t kSize = 0x100000;
} // namespace

Status validate_subaru_hitachi_sh7058_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruHitachiSh7058 || plan.target_id() != kProtocol || plan.mcu_name() != kMcu)
    {
        return fail(ErrorKind::InvalidConfig, "invalid SH7058 identity");
    }
    if (plan.operation() == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "SH7058 test write performs a live erase and is unsupported");
    }
    const bool read = plan.operation() == FlashOperation::Read;
    if (read ? (plan.transport() != TransportKind::Kline ||
                !std::holds_alternative<SubaruHitachiSh7058KlinePlan>(plan.family_plan()))
             : (plan.transport() != TransportKind::CanIso15765 ||
                !std::holds_alternative<SubaruHitachiSh7058CanPlan>(plan.family_plan())))
    {
        return fail(ErrorKind::InvalidConfig, "invalid SH7058 transport variant");
    }
    if (read)
    {
        const auto& p = std::get<SubaruHitachiSh7058KlinePlan>(plan.family_plan());
        if (p.initial_baud != 4800 || p.tester_id != 0xf0 || p.target_id != 0x10 || p.page_size != 0x80)
        {
            return fail(ErrorKind::InvalidConfig, "invalid SH7058 K-Line parameters");
        }
    }
    else
    {
        const auto& p = std::get<SubaruHitachiSh7058CanPlan>(plan.family_plan());
        if (p.bitrate != 500000 || p.request_id != 0x7e0 || p.response_id != 0x7e8 || p.extended_id ||
            p.frame_size != 0x100)
        {
            return fail(ErrorKind::InvalidConfig, "invalid SH7058 CAN parameters");
        }
    }
    if (plan.transfer_region().start != (read ? kSize : 0) || plan.transfer_region().length != kSize ||
        plan.image().has_value() != !read || (!read && plan.image()->size() != kSize) || plan.kernel().has_value() ||
        (!read && (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != 0 ||
                   plan.erase_regions()[0].length != kSize)) ||
        (read && !plan.erase_regions().empty()) || !plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "invalid SH7058 flash geometry");
    }
    return {};
}

Result<FlashPlan> build_subaru_hitachi_sh7058_plan(FlashOperation operation, std::string_view protocol,
                                                   std::string_view mcu, std::optional<bytes::Bytes> image)
{
    if (protocol != kProtocol || mcu != kMcu)
    {
        return fail(ErrorKind::InvalidConfig, "unsupported SH7058 protocol or MCU");
    }
    const flashdev_t *device = find_flash_device(kMcu);
    if (device == nullptr || device->romsize != kSize || device->numblocks != 1 || device->fblocks == nullptr ||
        device->fblocks[0].start != 0 || device->fblocks[0].len != kSize)
    {
        return fail(ErrorKind::InvalidConfig, "invalid SH7058 device geometry");
    }
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "SH7058 test write performs a live erase and is unsupported");
    }
    const bool read = operation == FlashOperation::Read;
    if (read ? image.has_value() : (!image.has_value() || image->size() != kSize))
    {
        return fail(ErrorKind::InvalidConfig, "SH7058 ROM image must be exactly 1 MiB for write");
    }
    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruHitachiSh7058,
        .transport = read ? TransportKind::Kline : TransportKind::CanIso15765,
        .target_id = std::string(protocol),
        .mcu_name = std::string(mcu),
        .transfer_region = {read ? kSize : 0, kSize},
        .erase_regions = read ? std::vector<MemoryRegion>{} : std::vector<MemoryRegion>{{0, kSize}},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = read ? FamilyPlan{SubaruHitachiSh7058KlinePlan{}} : FamilyPlan{SubaruHitachiSh7058CanPlan{}},
        .confirmations = {},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_hitachi_sh7058_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
