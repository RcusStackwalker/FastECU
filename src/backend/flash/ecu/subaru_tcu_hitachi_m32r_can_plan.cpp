#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"

#include <format>
#include <utility>

#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_types.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

constexpr std::string_view kProtocol = "sub_tcu_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB";
constexpr std::uint32_t kRomSize = 0x80000;

// Legacy read_mem/write_mem both operate on this single window (blocks 3-10
// of M32R_512KB's 11); read_mem additionally zero-pads the first 0x8000
// bytes back in before handing the image to the caller, so the transfer
// region and the image size agree at 0x80000 while the wire window itself
// covers only the trailing 0x78000 bytes -- see subaru_tcu_hitachi_m32r_can_types.h's
// divergence 2 and the CVT sibling, whose builder proves the core permits an
// image_size larger than the transfer region.
constexpr MemoryRegion kWindow{0x8000, 0x78000};

constexpr std::uint32_t kRequestId = 0x7E1;
constexpr std::uint32_t kResponseId = 0x7E9;
constexpr int kBitrate = 500000;
constexpr std::uint32_t kPageSize = 0x100;
constexpr std::uint32_t kWriteFrameSize = 128U;

Status validate_identity(std::string_view protocol, std::string_view mcu, const flashdev_t *& device)
{
    if (protocol != kProtocol)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Unsupported Subaru TCU Hitachi M32R CAN protocol: {}", protocol));
    }
    if (mcu != kMcu)
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("Subaru TCU Hitachi M32R CAN protocol {} requires MCU {}, not {}", protocol, kMcu, mcu));
    }
    device = find_flash_device(kMcu);
    if (device == nullptr || device->romsize != kRomSize || device->fblocks == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN flash geometry is invalid");
    }
    return {};
}

bool wire_parameters_match(const SubaruTcuHitachiM32rCanPlan& wire)
{
    return wire.request_id == kRequestId && wire.response_id == kResponseId && wire.bitrate == kBitrate &&
           !wire.extended_id && wire.page_size == kPageSize && wire.write_frame_size == kWriteFrameSize;
}

Status validate_regions(const FlashPlan& plan)
{
    if (plan.transfer_region().start != kWindow.start || plan.transfer_region().length != kWindow.length)
    {
        return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN transfer region is invalid");
    }
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.erase_regions().empty()
                   ? Status{}
                   : fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN read plans must not erase memory");
    }
    if (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != kWindow.start ||
        plan.erase_regions()[0].length != kWindow.length)
    {
        return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN erase region is invalid");
    }
    return {};
}

Status validate_image(const FlashPlan& plan)
{
    if (plan.operation() == FlashOperation::Read)
    {
        return plan.image().has_value()
                   ? fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN read plan carries a ROM image")
                   : Status{};
    }
    if (!plan.image().has_value() || plan.image()->size() != kRomSize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", kRomSize));
    }
    return {};
}

} // namespace

Status validate_subaru_tcu_hitachi_m32r_can_plan(const FlashPlan& plan)
{
    if (plan.family() != FlashFamily::SubaruTcuHitachiM32rCan || plan.transport() != TransportKind::CanIso15765)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Subaru TCU Hitachi M32R CAN");
    }
    const flashdev_t *device = nullptr;
    if (Status identity = validate_identity(plan.target_id(), plan.mcu_name(), device); !identity.has_value())
    {
        return identity;
    }
    const auto *wire = std::get_if<SubaruTcuHitachiM32rCanPlan>(&plan.family_plan());
    if (wire == nullptr || !wire_parameters_match(*wire))
    {
        return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN wire parameters are invalid");
    }
    if (plan.kernel().has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN plans are kernel-free");
    }
    // Safety-critical: the legacy reflash_block ignores its test_write_arg
    // parameter entirely, so "test write" performs the same live erase and
    // flash write as "write" -- see the divergence-1 comment in
    // subaru_tcu_hitachi_m32r_can_types.h. There is no dry-run to port.
    if (plan.operation() == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "test_write is not supported by Subaru TCU Hitachi M32R CAN");
    }
    if (!plan.confirmations().empty())
    {
        return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN plans must not declare extra confirmations");
    }
    if (Status regions = validate_regions(plan); !regions.has_value())
    {
        return regions;
    }
    return validate_image(plan);
}

Result<FlashPlan> build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                         std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    const flashdev_t *device = nullptr;
    if (Status identity = validate_identity(protocol_name, mcu_type, device); !identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    // Safety-critical: see the divergence-1 comment above and in
    // subaru_tcu_hitachi_m32r_can_types.h. Checked before the image checks
    // below so a TestWrite is rejected even without a ROM image supplied.
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported, "test_write is not supported by Subaru TCU Hitachi M32R CAN");
    }
    if (operation == FlashOperation::Read)
    {
        if (image.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "Subaru TCU Hitachi M32R CAN read plans must not carry an image");
        }
    }
    else if (!image.has_value() || image->size() != kRomSize)
    {
        return fail(ErrorKind::InvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes", kRomSize));
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruTcuHitachiM32rCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = kWindow,
        // T4b: decorative on the write path. write_rom() never reads
        // erase_regions() -- the wire erase is the chip-level
        // `31 02 01 FF FF FF FF` in executor.cpp's erase_flash(), which
        // takes no region argument at all. A future UI confirmation built on
        // this field would display 0x8000-0x80000, but the wire command's
        // actual erase scope is precisely the open question recorded in the
        // bench checklist's item 0 (blocks 3-10 only, or chip-wide).
        .erase_regions = operation == FlashOperation::Write ? std::vector{kWindow} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruTcuHitachiM32rCanPlan{.request_id = kRequestId,
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
    if (Status valid = validate_subaru_tcu_hitachi_m32r_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
