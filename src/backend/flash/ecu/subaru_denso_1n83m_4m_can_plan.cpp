#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_1n83m_4m_can"}};

// fblocks_N83M_4MB[1], the window legacy read_memory hardcodes over its own
// arguments (lines 834-836) and the 0x34/0x35 setup PDUs spell out literally
// (lines 845-860, 886-901).
constexpr MemoryRegion kMainBlock{0x08FAC000, 0x003D3F00};
constexpr std::uint32_t kImageStart = 0x08F9C000; // fblocks[0].start
constexpr std::uint32_t kImageSize = 0x3E4000;    // fblocks[0..2] summed, and N83M_4MB's own romsize
constexpr std::uint32_t kLeadPad = 0x10000;
constexpr std::uint32_t kTailPad = 0x100;

// Unlike its N83M_1_5MB sibling, N83M_4MB's declared romsize (0x3E4000) does
// equal its own fblocks sum, so it is checked here, as SH72531 checks its own.
// (SH72543d also checks romsize, but against its single-block table's declared
// 0x200000 rather than a block sum.) N83M_1_5MB remains the one family that
// cannot check it. Nothing on either path consumes romsize -- read_memory
// discards the length argument derived from it at line 836 -- so the check
// guards the flash table, not the transfer.
bool geometry_ok(const flashdev_t& device)
{
    return device.numblocks == 3 && device.romsize == kImageSize && device.fblocks[0].start == kImageStart &&
           device.fblocks[1].start == kMainBlock.start && device.fblocks[1].len == kMainBlock.length;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruDenso1n83m_4mCanPlan>(&plan.family_plan());
    return p != nullptr && p->request_id == 0x7e0 && p->response_id == 0x7e8 && p->bitrate == 500000 &&
           !p->extended_id && p->lead_pad_len == kLeadPad && p->tail_pad_len == kTailPad;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Denso 1N83M 4M CAN",
    .protocols = kProtocols,
    .mcu = "N83M_4MB",
    .family = FlashFamily::SubaruDenso1n83m_4mCan,
    .transport = TransportKind::CanIso15765,
    .read_region = kMainBlock,
    .write_region = kMainBlock,
    .image_size = kImageSize,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_denso_1n83m_4m_can_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_denso_1n83m_4m_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                       std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruDenso1n83m_4mCanPlan{0x7e0, 0x7e8, 500000, false, kLeadPad, kTailPad});
}
} // namespace fastecu::flash
