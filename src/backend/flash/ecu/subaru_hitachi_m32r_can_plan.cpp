#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_ecu_hitachi_m32r_can"}};

constexpr MemoryRegion kRom{0, 0x80000};

bool GeometryOk(const FlashDevice& device)
{
    return device.romsize == kRom.length && device.numblocks == 1 && device.fblocks[0].start == kRom.start &&
           device.fblocks[0].len == kRom.length;
}

bool WireParamsOk(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruHitachiM32rCanPlan>(&plan.FamilyPlan());
    return p != nullptr && p->request_id == 0x7e0 && p->response_id == 0x7e8 && p->bitrate == 500000 && !p->extended_id;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Hitachi M32R CAN",
    .protocols = kProtocols,
    .mcu = "M32R_512KB_1block",
    .family = FlashFamily::kSubaruHitachiM32rCan,
    .transport = TransportKind::kCanIso15765,
    .read_region = kRom,
    .write_region = kRom,
    .image_size = kRom.length,
    .geometry_ok = GeometryOk,
    .wire_params_ok = WireParamsOk,
};
} // namespace

Status ValidateSubaruHitachiM32rCanPlan(const FlashPlan& plan)
{
    return ValidateSingleWindowPlan(kSpec, plan);
}

Result<FlashPlan> BuildSubaruHitachiM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                                std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return BuildSingleWindowPlan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                 SubaruHitachiM32rCanPlan{0x7e0, 0x7e8, 500000, false});
}
} // namespace fastecu::flash
