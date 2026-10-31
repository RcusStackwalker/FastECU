#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"

#include <array>
#include <span>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

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
// covers only the trailing 0x78000 bytes -- see subaru_tcu_hitachi_m32r_can_plan.h's
// divergence 2 and the CVT sibling, whose builder proves the core permits an
// image_size larger than the transfer region.
constexpr MemoryRegion kWindow{0x8000, 0x78000};

bool GeometryOk(const FlashDevice& device)
{
    return device.romsize == kRomSize && device.fblocks != nullptr;
}

bool WireParamsOk(const FlashPlan& plan)
{
    const auto *p = plan.FamilyPlan().As<SubaruTcuHitachiM32rCanPlan>();
    return p != nullptr && p->request_id == 0x7E1 && p->response_id == 0x7E9 && p->bitrate == 500000 &&
           !p->extended_id && p->page_size == 0x100 && p->write_frame_size == 128U;
}

constexpr std::array kExportedProtocols{kProtocol};

// TestWrite is rejected (safety-critical): the legacy reflash_block ignores
// its test_write_arg parameter entirely, so "test write" would perform the
// same live erase and flash write as "write" -- see the divergence-1 comment
// in subaru_tcu_hitachi_m32r_can_plan.h. There is no dry-run to port.
//
// The write erase region is decorative on the write path (T4b): write_rom()
// never reads erase_regions() -- the wire erase is the chip-level
// `31 02 01 FF FF FF FF` in executor.cpp's erase_flash(), which takes no
// region argument at all. A future UI confirmation built on this field would
// display 0x8000-0x80000, but the wire command's actual erase scope is
// precisely the open question recorded in the bench checklist's item 0
// (blocks 3-10 only, or chip-wide).
constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru TCU Hitachi M32R CAN",
    .protocols = kExportedProtocols,
    .mcu = kMcu,
    .read_region = kWindow,
    .write_region = kWindow,
    .image_size = kRomSize,
    .geometry_ok = GeometryOk,
    .wire_params_ok = WireParamsOk,
};
} // namespace

Status ValidateSubaruTcuHitachiM32rCanPlan(const FlashPlan& plan)
{
    return ValidateSingleWindowPlan(kSpec, plan);
}

Result<FlashPlan> BuildSubaruTcuHitachiM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                   std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    // Unlike the other single-window families, which drop a read's image,
    // this family rejects one.
    if (operation == FlashOperation::kRead && image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru TCU Hitachi M32R CAN read plans must not carry an image");
    }
    return BuildSingleWindowPlan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                 SubaruTcuHitachiM32rCanPlan{0x7E1, 0x7E9, 500000, false, 0x100, 128U});
}

std::span<const std::string_view> SubaruTcuHitachiM32rCanProtocols()
{
    return kExportedProtocols;
}

Status PlanValidator<SubaruTcuHitachiM32rCanPlan>::Validate(const FlashPlan& plan)
{
    return ValidateSubaruTcuHitachiM32rCanPlan(plan);
}

} // namespace fastecu::flash
