#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_ecu_mitsu_m32r_kline"}};

constexpr MemoryRegion kUserspace{0x8000, 0x78000};
constexpr std::uint32_t kImageSize = 0x80000;

bool GeometryOk(const FlashDevice& device)
{
    constexpr std::array<FlashBlock, 4> kExpected{{{0, 0x4000}, {0x4000, 0x2000}, {0x6000, 0x2000}, {0x8000, 0x78000}}};
    if (device.romsize != kImageSize || device.numblocks != kExpected.size())
    {
        return false;
    }
    return std::ranges::all_of(
        std::views::iota(std::size_t{0}, kExpected.size()), [&](std::size_t i)
        { return device.fblocks[i].start == kExpected[i].start && device.fblocks[i].len == kExpected[i].len; });
}

bool WireParamsOk(const FlashPlan& plan)
{
    const auto *p = plan.FamilyPlan().As<SubaruMitsuM32rKlinePlan>();
    return p != nullptr && p->tester_id == 0xf0 && p->target_id == 0x10 && p->initial_baud == 4800 &&
           p->flash_baud == 15625 && p->chunk_size == 128 && p->unread_prefix_fill == 0xff;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Mitsubishi M32R K-Line",
    .protocols = kProtocols,
    .mcu = "M32R_512KB_4blocks",
    .family = FlashFamily::kSubaruMitsuM32rKline,
    .transport = TransportKind::kKline,
    .read_region = kUserspace,
    .write_region = kUserspace,
    .image_size = kImageSize,
    .geometry_ok = GeometryOk,
    .wire_params_ok = WireParamsOk,
};
} // namespace

Status ValidateSubaruMitsuM32rKlinePlan(const FlashPlan& plan)
{
    return ValidateSingleWindowPlan(kSpec, plan);
}

Result<FlashPlan> BuildSubaruMitsuM32rKlinePlan(FlashOperation operation, std::string_view protocol_name,
                                                std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return BuildSingleWindowPlan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                 SubaruMitsuM32rKlinePlan{0xf0, 0x10, 4800, 15625, 128, 0xff});
}

std::span<const std::string_view> SubaruMitsuM32rKlineProtocols()
{
    return kProtocols;
}

Status PlanValidator<SubaruMitsuM32rKlinePlan>::Validate(const FlashPlan& plan)
{
    return ValidateSubaruMitsuM32rKlinePlan(plan);
}

} // namespace fastecu::flash
