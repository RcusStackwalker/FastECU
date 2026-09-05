#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"

#include <array>
#include <cstddef>
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

bool geometry_ok(const flashdev_t& device)
{
    constexpr std::array<flashblock, 4> expected{{{0, 0x4000}, {0x4000, 0x2000}, {0x6000, 0x2000}, {0x8000, 0x78000}}};
    if (device.romsize != kImageSize || device.numblocks != expected.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        if (device.fblocks[i].start != expected[i].start || device.fblocks[i].len != expected[i].len)
        {
            return false;
        }
    }
    return true;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruMitsuM32rKlinePlan>(&plan.family_plan());
    return p != nullptr && p->tester_id == 0xf0 && p->target_id == 0x10 && p->initial_baud == 4800 &&
           p->flash_baud == 15625 && p->chunk_size == 128 && p->unread_prefix_fill == 0xff;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Mitsubishi M32R K-Line",
    .protocols = kProtocols,
    .mcu = "M32R_512KB_4blocks",
    .family = FlashFamily::SubaruMitsuM32rKline,
    .transport = TransportKind::Kline,
    .read_region = kUserspace,
    .write_region = kUserspace,
    .image_size = kImageSize,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_mitsu_m32r_kline_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_mitsu_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                     std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruMitsuM32rKlinePlan{0xf0, 0x10, 4800, 15625, 128, 0xff});
}
} // namespace fastecu::flash
