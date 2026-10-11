#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"

#include <format>
#include <utility>

#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_types.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

constexpr std::string_view kProtocol = "sub_ecu_hitachi_sh72543r_can";
constexpr std::string_view kMcu = "SH72543R";
constexpr std::uint32_t kRomSize = 0x200000;

constexpr MemoryRegion kReadWindow{0, 0x200000};
constexpr MemoryRegion kWriteWindow{0x6000, 0x1FA000};

constexpr std::uint32_t kRequestId = 0x7E0;
constexpr std::uint32_t kResponseId = 0x7E8;
constexpr int kBitrate = 500000;
constexpr std::uint32_t kPageSize = 0x400;
constexpr std::uint32_t kWriteFrameSize = 0x100U;

Status ValidateIdentity(std::string_view protocol, std::string_view mcu, const FlashDevice *& device)
{
    if (protocol != kProtocol && protocol != "sub_ecu_hitachi_sh72543r_can_recovery")
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Unsupported Subaru Hitachi SH72543R CAN protocol: {}", protocol));
    }
    if (mcu != kMcu)
    {
        return Fail(
            ErrorKind::kInvalidConfig,
            std::format("Subaru Hitachi SH72543R CAN protocol {} requires MCU {}, not {}", protocol, kMcu, mcu));
    }
    device = FindFlashDevice(kMcu);
    if (device == nullptr || device->romsize != kRomSize || device->fblocks == nullptr || device->numblocks != 2 ||
        device->fblocks[0].start != 0 || device->fblocks[0].len != 0x6000 || device->fblocks[1].start != 0x6000 ||
        device->fblocks[1].len != 0x1FA000)
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN flash geometry is invalid");
    }
    return {};
}

bool WireParametersMatch(const SubaruHitachiSh72543rCanPlan& wire)
{
    return wire.request_id == kRequestId && wire.response_id == kResponseId && wire.bitrate == kBitrate &&
           !wire.extended_id && wire.page_size == kPageSize && wire.write_frame_size == kWriteFrameSize;
}

Status ValidateRegions(const FlashPlan& plan)
{
    const auto window = plan.Operation() == FlashOperation::kRead ? kReadWindow : kWriteWindow;
    if (plan.TransferRegion().start != window.start || plan.TransferRegion().length != window.length)
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN transfer region is invalid");
    }
    if (plan.Operation() == FlashOperation::kRead)
    {
        return plan.EraseRegions().empty()
                   ? Status{}
                   : Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN read plans must not erase memory");
    }
    if (plan.EraseRegions().size() != 1 || plan.EraseRegions()[0].start != kWriteWindow.start ||
        plan.EraseRegions()[0].length != kWriteWindow.length)
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN erase region is invalid");
    }
    return {};
}

Status ValidateImage(const FlashPlan& plan)
{
    if (plan.Operation() == FlashOperation::kRead)
    {
        return plan.Image().has_value()
                   ? Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN read plan carries a ROM image")
                   : Status{};
    }
    if (!plan.Image().has_value() || plan.Image()->size() != kRomSize)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", kRomSize));
    }
    return {};
}

} // namespace

Status ValidateSubaruHitachiSh72543rCanPlan(const FlashPlan& plan)
{
    if (plan.Family() != FlashFamily::kSubaruHitachiSh72543rCan || plan.Transport() != TransportKind::kCanIso15765)
    {
        return Fail(ErrorKind::kInvalidConfig, "plan is not for Subaru Hitachi SH72543R CAN");
    }
    const FlashDevice *device = nullptr;
    if (Status identity = ValidateIdentity(plan.TargetId(), plan.McuName(), device); !identity.has_value())
    {
        return identity;
    }
    const auto *wire = std::get_if<SubaruHitachiSh72543rCanPlan>(&plan.FamilyPlan());
    if (wire == nullptr || !WireParametersMatch(*wire))
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN wire parameters are invalid");
    }
    if (plan.Kernel().has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN plans are kernel-free");
    }
    // Legacy operation.cpp:681: reflash_block ignores its test_write_arg
    // parameter entirely, so "test write" performs the same live erase and
    // flash write as "write" -- see the TestWrite comment in
    // subaru_hitachi_sh72543r_can_types.h. There is no dry-run to port.
    if (plan.Operation() != FlashOperation::kRead && plan.Operation() != FlashOperation::kWrite)
    {
        return Fail(ErrorKind::kUnsupported, "operation is not supported by Subaru Hitachi SH72543R CAN");
    }
    if (!plan.Confirmations().empty())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    "Subaru Hitachi SH72543R CAN plans must not declare extra confirmations");
    }
    if (Status regions = ValidateRegions(plan); !regions.has_value())
    {
        return regions;
    }
    return ValidateImage(plan);
}

Result<FlashPlan> BuildSubaruHitachiSh72543rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                    std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    const FlashDevice *device = nullptr;
    if (Status identity = ValidateIdentity(protocol_name, mcu_type, device); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    // Safety-critical: see the TestWrite policy above and in
    // subaru_hitachi_sh72543r_can_types.h. Checked before the image checks
    // below so a TestWrite is rejected even without a ROM image supplied.
    if (operation != FlashOperation::kRead && operation != FlashOperation::kWrite)
    {
        return Fail(ErrorKind::kUnsupported, "operation is not supported by Subaru Hitachi SH72543R CAN");
    }
    if (operation == FlashOperation::kRead)
    {
        if (image.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != kRomSize)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", kRomSize));
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::kSubaruHitachiSh72543rCan,
        .transport = TransportKind::kCanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = operation == FlashOperation::kRead ? kReadWindow : kWriteWindow,
        // Intended programming window; actual hardware erase scope is unqualified.
        .erase_regions = operation == FlashOperation::kWrite ? std::vector{kWriteWindow} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::kWrite ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiSh72543rCanPlan{.request_id = kRequestId,
                                                    .response_id = kResponseId,
                                                    .bitrate = kBitrate,
                                                    .extended_id = false,
                                                    .page_size = kPageSize,
                                                    .write_frame_size = kWriteFrameSize},
        .confirmations = {},
    };
    Result<FlashPlan> plan = ValidateAndBuild(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = ValidateSubaruHitachiSh72543rCanPlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
