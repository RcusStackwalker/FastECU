#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"

#include <array>
#include <span>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{

constexpr std::string_view kProtocol = "sub_ecu_hitachi_sh72543r_can";
constexpr std::array kProtocols{kProtocol, std::string_view{"sub_ecu_hitachi_sh72543r_can_recovery"}};
constexpr std::string_view kMcu = "SH72543R";
constexpr std::uint32_t kRomSize = 0x200000;

constexpr MemoryRegion kReadWindow{0, 0x200000};
constexpr MemoryRegion kWriteWindow{0x6000, 0x1FA000};

bool GeometryOk(const FlashDevice& device)
{
    return device.romsize == kRomSize && device.fblocks != nullptr && device.numblocks == 2 &&
           device.fblocks[0].start == 0 && device.fblocks[0].len == 0x6000 && device.fblocks[1].start == 0x6000 &&
           device.fblocks[1].len == 0x1FA000;
}

bool WireParamsOk(const FlashPlan& plan)
{
    const auto *p = plan.FamilyPlan().As<SubaruHitachiSh72543rCanPlan>();
    return p != nullptr && p->request_id == 0x7E0 && p->response_id == 0x7E8 && p->bitrate == 500000 &&
           !p->extended_id && p->page_size == 0x400 && p->write_frame_size == 0x100;
}

// TestWrite is rejected: reflash_block ignores its test_write_arg parameter
// entirely, so "test write" would perform the same live erase and flash write
// as "write" -- see subaru_hitachi_sh72543r_can_plan.h. There is no dry-run
// to port.
constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Hitachi SH72543R CAN",
    .protocols = kProtocols,
    .mcu = kMcu,
    .read_region = kReadWindow,
    .write_region = kWriteWindow,
    .image_size = kRomSize,
    .geometry_ok = GeometryOk,
    .wire_params_ok = WireParamsOk,
};
} // namespace

Status ValidateSubaruHitachiSh72543rCanPlan(const FlashPlan& plan)
{
    return ValidateSingleWindowPlan(kSpec, plan);
}

Result<FlashPlan> BuildSubaruHitachiSh72543rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                    std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    // Unlike the other single-window families, which drop a read's image,
    // this family rejects one.
    if (operation == FlashOperation::kRead && image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "Subaru Hitachi SH72543R CAN read plans must not carry an image");
    }
    return BuildSingleWindowPlan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                 SubaruHitachiSh72543rCanPlan{0x7E0, 0x7E8, 500000, false, 0x400, 0x100});
}

std::span<const std::string_view> SubaruHitachiSh72543rCanProtocols()
{
    return kProtocols;
}

Status PlanValidator<SubaruHitachiSh72543rCanPlan>::Validate(const FlashPlan& plan)
{
    return ValidateSubaruHitachiSh72543rCanPlan(plan);
}

} // namespace fastecu::flash
