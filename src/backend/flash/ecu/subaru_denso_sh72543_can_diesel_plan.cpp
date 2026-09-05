#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_sh72543_can_diesel"}};

// fblocks_SH72543d[0]. Unlike its three siblings, this family's read_memory
// does NOT hardcode over its own arguments -- the start_addr/length overwrite
// is commented out (legacy lines 813-814) -- so the caller's
// fblocks[0].start/len (legacy line 74) reach it, and the 0x34/0x35 setup
// PDUs are computed from them rather than spelled out literally (legacy lines
// 826-842 and 865-881). The values land on the same region either way; the
// executor's read_memory comment carries the same account.
constexpr MemoryRegion kMainBlock{0x00008000, 0x001F7F00};
// SH72543d's own romsize. Unlike its three siblings this is not the fblocks
// sum -- the table's 0x0-0x8000 entry is commented out -- so the image base
// (0x0) is not fblocks[0].start here; it is expressed in the executor, at the
// write-indexing site that is the only place it is used.
constexpr std::uint32_t kImageSize = 0x200000;
constexpr std::uint32_t kLeadPad = 0x8000;
constexpr std::uint32_t kTailPad = 0x100;

// SH72543d's flash table has a single block: fblocks_SH72543d[0] == {0x8000,
// 0x1F7F00}, so it is checked here.
bool geometry_ok(const flashdev_t& device)
{
    return device.numblocks == 1 && device.romsize == kImageSize && device.fblocks[0].start == kMainBlock.start &&
           device.fblocks[0].len == kMainBlock.length;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruDensoSh72543CanDieselPlan>(&plan.family_plan());
    return p != nullptr && p->request_id == 0x7e0 && p->response_id == 0x7e8 && p->bitrate == 500000 &&
           !p->extended_id && p->lead_pad_len == kLeadPad && p->tail_pad_len == kTailPad;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Denso SH72543 Diesel CAN",
    .protocols = kProtocols,
    .mcu = "SH72543d",
    .family = FlashFamily::SubaruDensoSh72543CanDiesel,
    .transport = TransportKind::CanIso15765,
    .read_region = kMainBlock,
    .write_region = kMainBlock,
    .image_size = kImageSize,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_denso_sh72543_can_diesel_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_denso_sh72543_can_diesel_plan(FlashOperation operation, std::string_view protocol_name,
                                                             std::string_view mcu_type,
                                                             std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruDensoSh72543CanDieselPlan{0x7e0, 0x7e8, 500000, false, kLeadPad, kTailPad});
}
} // namespace fastecu::flash
