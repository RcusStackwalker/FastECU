#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_types.h"

#include <array>
#include <span>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_1n83m_1_5m_can"}};

// kFlashBlocksN83M_1_5MB[1], the window legacy read_memory hardcodes over its own
// arguments and the 0x34/0x35 setup PDUs spell out literally.
constexpr MemoryRegion kMainBlock{0x08FAC000, 0x00173F00};
constexpr std::uint32_t kImageStart =
    DensoIso15765BootloaderProfileOf<SubaruDenso1n83m_1_5mCanPlan>::kValue.image_start; // fblocks[0].start
constexpr std::uint32_t kImageSize = 0x184000;                                          // fblocks[0..2] summed
constexpr std::uint32_t kLeadPad = 0x10000;
constexpr std::uint32_t kTailPad = 0x100;

// romsize is deliberately NOT checked: N83M_1_5MB declares 0x174000 while its
// own fblocks sum to 0x184000, and nothing on either the read or the write
// path consumes romsize (read_memory discards the length argument derived from
// it). See the original port design's "Read-image layout is already correct" section.
bool GeometryOk(const FlashDevice& device)
{
    return device.numblocks == 3 && device.fblocks[0].start == kImageStart &&
           device.fblocks[1].start == kMainBlock.start && device.fblocks[1].len == kMainBlock.length;
}

bool WireParamsOk(const FlashPlan& plan)
{
    const auto *p = plan.FamilyPlan().As<SubaruDenso1n83m_1_5mCanPlan>();
    return p != nullptr && p->request_id == 0x7e0 && p->response_id == 0x7e8 && p->bitrate == 500000 &&
           !p->extended_id && p->lead_pad_len == kLeadPad && p->tail_pad_len == kTailPad;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Denso 1N83M 1.5M CAN",
    .protocols = kProtocols,
    .mcu = "N83M_1_5MB",
    .read_region = kMainBlock,
    .write_region = kMainBlock,
    .image_size = kImageSize,
    .geometry_ok = GeometryOk,
    .wire_params_ok = WireParamsOk,
};
} // namespace

Status ValidateSubaruDenso1n83m15mCanPlan(const FlashPlan& plan)
{
    return ValidateSingleWindowPlan(kSpec, plan);
}

Result<FlashPlan> BuildSubaruDenso1n83m15mCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                  std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return BuildSingleWindowPlan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                 SubaruDenso1n83m_1_5mCanPlan{0x7e0, 0x7e8, 500000, false, kLeadPad, kTailPad});
}

std::span<const std::string_view> SubaruDenso1n83m15mCanProtocols()
{
    return kProtocols;
}

Status PlanValidator<SubaruDenso1n83m_1_5mCanPlan>::Validate(const FlashPlan& plan)
{
    return ValidateSubaruDenso1n83m15mCanPlan(plan);
}

} // namespace fastecu::flash
