#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_tcu_cvt_mitsu_mh8104_can"}};

// Legacy read_mem hardcodes start_addr = 0x8000, length = 0x78000
// unconditionally (lines 364-366, "hack for testing" -- overwriting whatever
// the caller passed), the same hardcoding shape as the sibling MH8111 family.
constexpr MemoryRegion kReadRegion{0x8000, 0x78000};

// Legacy write_mem's block_modified mask ({0,0,0,1}, only numblocks=4
// entries relevant) skips blocks 0-2 and flashes only block 3 --
// fblocks_MH8104[3] = {0x8000, 0x78000} (kernelmemorymodels.h). Unlike
// MH8111 (whose read window and sole flashed block do NOT overlap), this
// family's write window is IDENTICAL to kReadRegion -- both are
// fblocks_MH8104[3], a genuine per-family difference confirmed directly
// against kernelmemorymodels.h, not an assumption carried over from the
// sibling family.
constexpr MemoryRegion kWriteRegion{0x8000, 0x78000};

// flashdevices[MH8104].romsize = 512*1024 = 0x80000 -- the family's true
// declared capacity (MH8104's real capacity, unlike MH8111's 0x180000).
// write_mem loads/encrypts the FULL image (ecuCalDef->FullRomData) and
// reflash_block indexes it at absolute offsets up to fdt->fblocks[3].start +
// fdt->fblocks[3].len == 0x80000, even though the flashed window is the same
// size as the image's own upper 0x78000 bytes.
constexpr std::uint32_t kImageSize = 0x80000;

// Check block 0 (full-table sanity) and block 3 specifically -- block 3 is
// both this family's read window AND the block it writes.
bool geometry_ok(const flashdev_t& device)
{
    return device.romsize == kImageSize && device.numblocks == 4 && device.fblocks[0].start == 0 &&
           device.fblocks[0].len == 0x4000 && device.fblocks[3].start == kWriteRegion.start &&
           device.fblocks[3].len == kWriteRegion.length;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruTcuCvtMitsuMh8104CanPlan>(&plan.family_plan());
    return p != nullptr && p->request_id == 0x7e1 && p->response_id == 0x7e9 && p->bitrate == 500000 && !p->extended_id;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru TCU CVT Mitsu MH8104 CAN",
    .protocols = kProtocols,
    .mcu = "MH8104",
    .family = FlashFamily::SubaruTcuCvtMitsuMh8104Can,
    .transport = TransportKind::CanIso15765,
    .read_region = kReadRegion,
    .write_region = kWriteRegion,
    .image_size = kImageSize,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_tcu_cvt_mitsu_mh8104_can_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                             std::string_view mcu_type,
                                                             std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruTcuCvtMitsuMh8104CanPlan{0x7e1, 0x7e9, 500000, false});
}
} // namespace fastecu::flash
