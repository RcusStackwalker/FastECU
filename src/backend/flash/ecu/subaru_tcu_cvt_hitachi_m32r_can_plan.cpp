#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_tcu_cvt_hitachi_m32r_can"}};

// Legacy read_mem computes start_addr = 0 - 0x00100000, which underflows
// uint32_t to 0xFFF00000 and bypasses read_mem's own "< 0x8000" floor clamp
// entirely (0xFFF00000 is not less than 0x8000) -- that path never executed
// on real hardware (execute() called hack_words(), never read_mem()). This
// plan targets the clamp's evident intent instead of the never-observed
// underflowed address; see the design's dead-code decision and Task 3's
// second deliberate divergence. Read and write share the same window: the
// same 8 flash blocks (index 3-10 of M32R_512KB's 11) are both dumped and
// reflashed.
constexpr MemoryRegion kReadRegion{0x8000, 0x78000};
constexpr MemoryRegion kWriteRegion{0x8000, 0x78000};
// Legacy write_mem loads/writes the full, unclamped ROM image
// (ecuCalDef->FullRomData, romsize bytes) even though only kWriteRegion of
// it is ever transmitted -- the same full size read_mem's 0x8000-zero-pad +
// dumped-payload result produces.
constexpr std::uint32_t kImageSize = 0x80000;

// Spot-check block 0 only, matching subaru_hitachi_m32r_kline_plan.cpp's
// precedent -- not every one of M32R_512KB's 11 blocks.
bool geometry_ok(const flashdev_t& device)
{
    return device.romsize == kImageSize && device.numblocks == 11 && device.fblocks[0].start == 0 &&
           device.fblocks[0].len == 0x4000;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruTcuCvtHitachiM32rCanPlan>(&plan.family_plan());
    return p != nullptr && p->request_id == 0x7e1 && p->response_id == 0x7e9 && p->bitrate == 500000 && !p->extended_id;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru TCU CVT Hitachi M32R CAN",
    .protocols = kProtocols,
    .mcu = "M32R_512KB",
    .family = FlashFamily::SubaruTcuCvtHitachiM32rCan,
    .transport = TransportKind::CanIso15765,
    .read_region = kReadRegion,
    .write_region = kWriteRegion,
    .image_size = kImageSize,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_tcu_cvt_hitachi_m32r_can_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                             std::string_view mcu_type,
                                                             std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruTcuCvtHitachiM32rCanPlan{0x7e1, 0x7e9, 500000, false});
}
} // namespace fastecu::flash
