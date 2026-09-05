#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_1n83m_1_5m_can"}};

// fblocks_N83M_1_5MB[1], the window legacy read_memory hardcodes over its own
// arguments (lines 826-828) and the 0x34/0x35 setup PDUs spell out literally
// (lines 842-852, 883-893).
constexpr MemoryRegion kMainBlock{0x08FAC000, 0x00173F00};
constexpr std::uint32_t kImageStart = 0x08F9C000; // fblocks[0].start
constexpr std::uint32_t kImageSize = 0x184000;    // fblocks[0..2] summed
constexpr std::uint32_t kLeadPad = 0x10000;
constexpr std::uint32_t kTailPad = 0x100;

// romsize is deliberately NOT checked: N83M_1_5MB declares 0x174000 while its
// own fblocks sum to 0x184000, and nothing on either the read or the write
// path consumes romsize (read_memory discards the length argument derived from
// it). See the wave-4 design's "Read-image layout is already correct" section.
bool geometry_ok(const flashdev_t& device)
{
    return device.numblocks == 3 && device.fblocks[0].start == kImageStart &&
           device.fblocks[1].start == kMainBlock.start && device.fblocks[1].len == kMainBlock.length;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruDenso1n83m_1_5mCanPlan>(&plan.family_plan());
    return p != nullptr && p->request_id == 0x7e0 && p->response_id == 0x7e8 && p->bitrate == 500000 &&
           !p->extended_id && p->lead_pad_len == kLeadPad && p->tail_pad_len == kTailPad;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Denso 1N83M 1.5M CAN",
    .protocols = kProtocols,
    .mcu = "N83M_1_5MB",
    .family = FlashFamily::SubaruDenso1n83m_1_5mCan,
    .transport = TransportKind::CanIso15765,
    .read_region = kMainBlock,
    .write_region = kMainBlock,
    .image_size = kImageSize,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_denso_1n83m_1_5m_can_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                         std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruDenso1n83m_1_5mCanPlan{0x7e0, 0x7e8, 500000, false, kLeadPad, kTailPad});
}
} // namespace fastecu::flash
