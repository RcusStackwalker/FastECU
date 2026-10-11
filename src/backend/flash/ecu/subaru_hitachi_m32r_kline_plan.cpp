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
constexpr HitachiM32rKlineSessionMode ModeFor(std::string_view protocol)
{
    return protocol == kRecovery ? HitachiM32rKlineSessionMode::kRecovery : HitachiM32rKlineSessionMode::kNormal;
}

bool GeometryOk(const FlashDevice& device)
{
    return device.romsize == kRom.length && device.numblocks == 1 && device.fblocks[0].start == kRom.start &&
           device.fblocks[0].len == kRom.length;
}

bool WireParamsOk(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruHitachiM32rKlinePlan>(&plan.FamilyPlan());
    return p != nullptr && p->session_mode == ModeFor(plan.TargetId()) && p->tester_id == 0xf0 &&
           p->target_id == 0x10 && p->initial_baud == 4800 && p->write_baud == 15625 && p->read_baud == 38400 &&
           p->chunk_size == 128 && p->read_address_bias == 0x100000;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Hitachi M32R K-Line",
    .protocols = kProtocols,
    .mcu = "M32R_512KB_1block",
    .family = FlashFamily::kSubaruHitachiM32rKline,
    .transport = TransportKind::kKline,
    .read_region = kRom,
    .write_region = kRom,
    .image_size = kRom.length,
    .geometry_ok = GeometryOk,
    .wire_params_ok = WireParamsOk,
};
} // namespace

Status ValidateSubaruHitachiM32rKlinePlan(const FlashPlan& plan)
{
    return ValidateSingleWindowPlan(kSpec, plan);
}

Result<FlashPlan> BuildSubaruHitachiM32rKlinePlan(FlashOperation operation, std::string_view protocol_name,
                                                  std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return BuildSingleWindowPlan(
        kSpec, operation, protocol_name, mcu_type, std::move(image),
        SubaruHitachiM32rKlinePlan{ModeFor(protocol_name), 0xf0, 0x10, 4800, 15625, 38400, 128, 0x100000});
}
} // namespace fastecu::flash
