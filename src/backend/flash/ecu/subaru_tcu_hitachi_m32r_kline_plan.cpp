#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::string_view kProtocol = "sub_tcu_hitachi_m32r_kline";
constexpr std::array kProtocols{kProtocol};

constexpr MemoryRegion kRom{0, 0x80000};

bool geometry_ok(const flashdev_t& device)
{
    return device.romsize == kRom.length && device.fblocks[0].start == kRom.start;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruTcuHitachiM32rKlinePlan>(&plan.family_plan());
    return p != nullptr && p->tester_id == 0xf0 && p->target_id == 0x18 && p->baud == 4800 && p->block_size == 96;
}

// write_region and image_size are unreachable for this family -- supports_write
// is false -- but the shared spec requires both; they mirror read_region so a
// future write path would not inherit a wrong window by default.
constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru TCU Hitachi M32R K-Line",
    .protocols = kProtocols,
    .mcu = "M32R_512KB",
    .family = FlashFamily::SubaruTcuHitachiM32rKline,
    .transport = TransportKind::Kline,
    .read_region = kRom,
    .write_region = kRom,
    .image_size = kRom.length,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
    .supports_write = false,
};
} // namespace

Status validate_subaru_tcu_hitachi_m32r_kline_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                           std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruTcuHitachiM32rKlinePlan{0xf0, 0x18, 4800, 96});
}
} // namespace fastecu::flash
