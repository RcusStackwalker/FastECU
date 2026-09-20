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

Status validate_identity(std::string_view protocol, std::string_view mcu, const flashdev_t *& device)
{
    if (protocol != kProtocol && protocol != "sub_ecu_hitachi_sh72543r_can_recovery")
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Unsupported Subaru Hitachi SH72543R CAN protocol: {}", protocol));
    }
    if (mcu != kMcu)
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("Subaru Hitachi SH72543R CAN protocol {} requires MCU {}, not {}", protocol, kMcu, mcu));
    }
    device = find_flash_device(kMcu);
    if (device == nullptr || device->romsize != kRomSize || device->fblocks == nullptr || device->numblocks != 2 ||
        device->fblocks[0].start != 0 || device->fblocks[0].len != 0x6000 || device->fblocks[1].start != 0x6000 ||
        device->fblocks[1].len != 0x1FA000)
    {
        return fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN flash geometry is invalid");
    }
    return {};
}

bool wire_parameters_match(const SubaruHitachiSh72543rCanPlan& wire)
{
    return wire.request_id == kRequestId && wire.response_id == kResponseId && wire.bitrate == kBitrate &&
           !wire.extended_id && wire.page_size == kPageSize && wire.write_frame_size == kWriteFrameSize;
}

Status validate_regions(const FlashPlan& plan)
{
    const auto window = plan.operation() == FlashOperation::Read ? kReadWindow : kWriteWindow;
    if (plan.transfer_region().start != window.start || plan.transfer_region().length != window.length)
    {
        return fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN transfer region is invalid");
    }
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.erase_regions().empty()
                   ? Status{}
                   : fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN read plans must not erase memory");
    }
    if (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != kWriteWindow.start ||
        plan.erase_regions()[0].length != kWriteWindow.length)
    {
        return fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN erase region is invalid");
    }
    return {};
}

Status validate_image(const FlashPlan& plan)
{
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.image().has_value()
                   ? fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN read plan carries a ROM image")
                   : Status{};
    }
    if (!plan.image().has_value() || plan.image()->size() != kRomSize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", kRomSize));
    }
    return {};
}

} // namespace

Status validate_subaru_hitachi_sh72543r_can_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruHitachiSh72543rCan || plan.transport() != TransportKind::CanIso15765)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru Hitachi SH72543R CAN");
    }
    const flashdev_t *device = nullptr;
    if (Status identity = validate_identity(plan.target_id(), plan.mcu_name(), device); !identity.has_value())
    {
        return identity;
    }
    const auto *wire = std::get_if<SubaruHitachiSh72543rCanPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_parameters_match(*wire))
    {
        return fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN wire parameters are invalid");
    }
    if (plan.kernel().has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN plans are kernel-free");
    }
    // Legacy operation.cpp:681: reflash_block ignores its test_write_arg
    // parameter entirely, so "test write" performs the same live erase and
    // flash write as "write" -- see the divergence-1 comment in
    // subaru_hitachi_sh72543r_can_types.h. There is no dry-run to port.
    if (plan.operation() != FlashOperation::Read && plan.operation() != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported, "operation is not supported by Subaru Hitachi SH72543R CAN");
    }
    if (!plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN plans must not declare extra confirmations");
    }
    if (Status regions = validate_regions(plan); !regions.has_value())
    {
        return regions;
    }
    return validate_image(plan);
}

Result<FlashPlan> build_subaru_hitachi_sh72543r_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                         std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    const flashdev_t *device = nullptr;
    if (Status identity = validate_identity(protocol_name, mcu_type, device); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    // Safety-critical: see the TestWrite policy above and in
    // subaru_hitachi_sh72543r_can_types.h. Checked before the image checks
    // below so a TestWrite is rejected even without a ROM image supplied.
    if (operation != FlashOperation::Read && operation != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported, "operation is not supported by Subaru Hitachi SH72543R CAN");
    }
    if (operation == FlashOperation::Read)
    {
        if (image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "Subaru Hitachi SH72543R CAN read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != kRomSize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", kRomSize));
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruHitachiSh72543rCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = operation == FlashOperation::Read ? kReadWindow : kWriteWindow,
        // Intended programming window; actual hardware erase scope is unqualified.
        .erase_regions = operation == FlashOperation::Write ? std::vector{kWriteWindow} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiSh72543rCanPlan{.request_id = kRequestId,
                                                    .response_id = kResponseId,
                                                    .bitrate = kBitrate,
                                                    .extended_id = false,
                                                    .page_size = kPageSize,
                                                    .write_frame_size = kWriteFrameSize},
        .confirmations = {},
    };
    Result<FlashPlan> plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_subaru_hitachi_sh72543r_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
