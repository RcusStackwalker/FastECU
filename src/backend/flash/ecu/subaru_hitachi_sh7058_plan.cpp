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

Status ValidateSubaruHitachiSh7058Plan(const FlashPlan& plan)
{
    if (plan.Family() != FlashFamily::kSubaruHitachiSh7058 || plan.TargetId() != kProtocol || plan.McuName() != kMcu)
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 identity");
    }
    if (plan.Operation() == FlashOperation::kTestWrite)
    {
        return Fail(ErrorKind::kUnsupported, "SH7058 test write performs a live erase and is unsupported");
    }
    const bool read = plan.Operation() == FlashOperation::kRead;
    if (read ? (plan.Transport() != TransportKind::kKline ||
                !std::holds_alternative<SubaruHitachiSh7058KlinePlan>(plan.FamilyPlan()))
             : (plan.Transport() != TransportKind::kCanIso15765 ||
                !std::holds_alternative<SubaruHitachiSh7058CanPlan>(plan.FamilyPlan())))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 transport variant");
    }
    if (read)
    {
        const auto& p = std::get<SubaruHitachiSh7058KlinePlan>(plan.FamilyPlan());
        if (p.initial_baud != 4800 || p.tester_id != 0xf0 || p.target_id != 0x10 || p.page_size != 0x80)
        {
            return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 K-Line parameters");
        }
    }
    else
    {
        const auto& p = std::get<SubaruHitachiSh7058CanPlan>(plan.FamilyPlan());
        if (p.bitrate != 500000 || p.request_id != 0x7e0 || p.response_id != 0x7e8 || p.extended_id ||
            p.frame_size != 0x100)
        {
            return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 CAN parameters");
        }
    }
    if (plan.TransferRegion().start != (read ? kSize : 0) || plan.TransferRegion().length != kSize ||
        plan.Image().has_value() != !read || (!read && plan.Image()->size() != kSize) || plan.Kernel().has_value() ||
        (!read && (plan.EraseRegions().size() != 1 || plan.EraseRegions()[0].start != 0 ||
                   plan.EraseRegions()[0].length != kSize)) ||
        (read && !plan.EraseRegions().empty()))
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 flash geometry");
    }
    const auto confirmations = plan.Confirmations();
    const bool exactly_start_read = confirmations.size() == 1 &&
                                    confirmations.front().id == ConfirmationSpec::Id::kStartKlineRead &&
                                    confirmations.front().arguments.empty();
    if (read ? !exactly_start_read : !confirmations.empty())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    "SH7058 Read requires exactly the StartKlineRead confirmation; Write requires none");
    }
    return {};
}

Result<FlashPlan> BuildSubaruHitachiSh7058Plan(FlashOperation operation, std::string_view protocol,
                                               std::string_view mcu, std::optional<bytes::Bytes> image)
{
    if (protocol != kProtocol || mcu != kMcu)
    {
        return Fail(ErrorKind::kInvalidConfig, "unsupported SH7058 protocol or MCU");
    }
    const FlashDevice *device = FindFlashDevice(kMcu);
    if (device == nullptr || device->romsize != kSize || device->numblocks != 1 || device->fblocks == nullptr ||
        device->fblocks[0].start != 0 || device->fblocks[0].len != kSize)
    {
        return Fail(ErrorKind::kInvalidConfig, "invalid SH7058 device geometry");
    }
    if (operation == FlashOperation::kTestWrite)
    {
        return Fail(ErrorKind::kUnsupported, "SH7058 test write performs a live erase and is unsupported");
    }
    const bool read = operation == FlashOperation::kRead;
    if (read ? image.has_value() : (!image.has_value() || image->size() != kSize))
    {
        return Fail(ErrorKind::kInvalidConfig, "SH7058 ROM image must be exactly 1 MiB for write");
    }
    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::kSubaruHitachiSh7058,
        .transport = read ? TransportKind::kKline : TransportKind::kCanIso15765,
        .target_id = std::string(protocol),
        .mcu_name = std::string(mcu),
        .transfer_region = {read ? kSize : 0, kSize},
        .erase_regions = read ? std::vector<MemoryRegion>{} : std::vector<MemoryRegion>{{0, kSize}},
        .image = std::move(image),
        .kernel = std::nullopt,
        .family_plan = read ? FamilyPlan{SubaruHitachiSh7058KlinePlan{}} : FamilyPlan{SubaruHitachiSh7058CanPlan{}},
        .confirmations =
            read ? std::vector<ConfirmationSpec>{ConfirmationSpec{.id = ConfirmationSpec::Id::kStartKlineRead}}
                 : std::vector<ConfirmationSpec>{},
    };
    auto plan = ValidateAndBuild(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = ValidateSubaruHitachiSh7058Plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
