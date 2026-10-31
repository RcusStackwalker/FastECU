#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_types.h"

#include <array>
#include <span>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_sh72531_can"}};

// kFlashBlocksSH72531[1], the window legacy read_memory hardcodes over its own
// arguments and the 0x34/0x35 setup PDUs spell out literally.
constexpr MemoryRegion kMainBlock{0x00008000, 0x00137F00};
constexpr std::uint32_t kImageStart =
    DensoIso15765BootloaderProfileOf<SubaruDensoSh72531CanPlan>::kValue.image_start; // fblocks[0].start
constexpr std::uint32_t kImageSize = 0x140000; // fblocks[0..2] summed, and SH72531's own romsize
constexpr std::uint32_t kLeadPad = 0x8000;
constexpr std::uint32_t kTailPad = 0x100;

// Unlike its N83M_1_5MB sibling, SH72531's flash table is self-consistent:
// romsize (1280 KiB) equals its own fblocks sum, so it is checked here.
bool GeometryOk(const FlashDevice& device)
{
    return device.numblocks == 3 && device.romsize == kImageSize && device.fblocks[0].start == kImageStart &&
           device.fblocks[1].start == kMainBlock.start && device.fblocks[1].len == kMainBlock.length;
}

bool WireParamsOk(const FlashPlan& plan)
{
    const auto *p = plan.FamilyPlan().As<SubaruDensoSh72531CanPlan>();
    return p != nullptr && p->request_id == 0x7e0 && p->response_id == 0x7e8 && p->bitrate == 500000 &&
           !p->extended_id && p->lead_pad_len == kLeadPad && p->tail_pad_len == kTailPad;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Denso SH72531 CAN",
    .protocols = kProtocols,
    .mcu = "SH72531",
    .family = FlashFamily::kSubaruDensoSh72531Can,
    .transport = TransportKind::kCanIso15765,
    .read_region = kMainBlock,
    .write_region = kMainBlock,
    .image_size = kImageSize,
    .geometry_ok = GeometryOk,
    .wire_params_ok = WireParamsOk,
};
} // namespace

Status ValidateSubaruDensoSh72531CanPlan(const FlashPlan& plan)
{
    return ValidateSingleWindowPlan(kSpec, plan);
}

Result<FlashPlan> BuildSubaruDensoSh72531CanPlan(FlashOperation operation, std::string_view protocol_name,
                                                 std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return BuildSingleWindowPlan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                 SubaruDensoSh72531CanPlan{0x7e0, 0x7e8, 500000, false, kLeadPad, kTailPad});
}

std::span<const std::string_view> SubaruDensoSh72531CanProtocols()
{
    return kProtocols;
}

Status PlanValidator<SubaruDensoSh72531CanPlan>::Validate(const FlashPlan& plan)
{
    return ValidateSubaruDensoSh72531CanPlan(plan);
}

} // namespace fastecu::flash
