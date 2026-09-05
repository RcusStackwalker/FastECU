#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::string_view kNormal = "sub_ecu_hitachi_m32r_kline";
constexpr std::string_view kRecovery = "sub_ecu_hitachi_m32r_kline_recovery";
constexpr std::array kProtocols{kNormal, kRecovery};

constexpr MemoryRegion kRom{0, 0x80000};

// The session mode is not a free parameter: it is decided by which of the two
// protocol names the caller used, so a plan whose mode disagrees with its own
// target_id is malformed.
constexpr HitachiM32rKlineSessionMode mode_for(std::string_view protocol)
{
    return protocol == kRecovery ? HitachiM32rKlineSessionMode::Recovery : HitachiM32rKlineSessionMode::Normal;
}

bool geometry_ok(const flashdev_t& device)
{
    return device.romsize == kRom.length && device.numblocks == 1 && device.fblocks[0].start == kRom.start &&
           device.fblocks[0].len == kRom.length;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruHitachiM32rKlinePlan>(&plan.family_plan());
    return p != nullptr && p->session_mode == mode_for(plan.target_id()) && p->tester_id == 0xf0 &&
           p->target_id == 0x10 && p->initial_baud == 4800 && p->write_baud == 15625 && p->read_baud == 38400 &&
           p->chunk_size == 128 && p->read_address_bias == 0x100000;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Hitachi M32R K-Line",
    .protocols = kProtocols,
    .mcu = "M32R_512KB_1block",
    .family = FlashFamily::SubaruHitachiM32rKline,
    .transport = TransportKind::Kline,
    .read_region = kRom,
    .write_region = kRom,
    .image_size = kRom.length,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_hitachi_m32r_kline_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_hitachi_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                       std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(
        kSpec, operation, protocol_name, mcu_type, std::move(image),
        SubaruHitachiM32rKlinePlan{mode_for(protocol_name), 0xf0, 0x10, 4800, 15625, 38400, 128, 0x100000});
}
} // namespace fastecu::flash
