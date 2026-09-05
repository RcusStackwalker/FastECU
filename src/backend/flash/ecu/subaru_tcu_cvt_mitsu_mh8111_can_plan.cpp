#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"

#include <array>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/single_window_plan.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_tcu_cvt_mitsu_mh8111_can"}};

// Legacy read_mem hardcodes start_addr = 0x8000, length = 0x78000
// unconditionally (lines 356-358, "hack for testing" -- overwriting
// whatever the caller passed), independent of block_modified/fblocks.
constexpr MemoryRegion kReadRegion{0x8000, 0x78000};

// Legacy write_mem's block_modified mask ({0,0,0,1}, only numblocks=4
// entries relevant) skips blocks 0-2 and flashes only block 3 --
// fblocks_MH8111[3] = {0x80000, 0x100000} (kernelmemorymodels.h). This does
// NOT overlap kReadRegion ({0x8000, 0x78000} ends at 0x80000, exactly where
// kWriteRegion begins) -- a genuine legacy asymmetry (read is an
// incomplete diagnostic dump of the low region; write assumes a full
// 0x180000-byte user-supplied ROM image and only ever reflashes the
// top block), preserved exactly rather than "fixed" into symmetry.
constexpr MemoryRegion kWriteRegion{0x80000, 0x100000};

// flashdevices[MH8111].romsize = 3*512*1024 = 0x180000 -- the family's true
// declared capacity. write_mem loads/encrypts the FULL image
// (ecuCalDef->FullRomData) and reflash_block indexes it at absolute offsets
// up to 0x180000 (fdt->fblocks[3].start + fdt->fblocks[3].len), even though
// only kWriteRegion of it is ever transmitted to the TCU.
constexpr std::uint32_t kImageSize = 0x180000;

// Check block 0 (full-table sanity) and block 3 specifically -- block 3,
// not block 0, is the block this family actually writes (see
// kWriteRegion above).
bool geometry_ok(const flashdev_t& device)
{
    return device.romsize == kImageSize && device.numblocks == 4 && device.fblocks[0].start == 0 &&
           device.fblocks[0].len == 0x40000 && device.fblocks[3].start == kWriteRegion.start &&
           device.fblocks[3].len == kWriteRegion.length;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruTcuCvtMitsuMh8111CanPlan>(&plan.family_plan());
    return p != nullptr && p->request_id == 0x7e1 && p->response_id == 0x7e9 && p->bitrate == 500000 && !p->extended_id;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru TCU CVT Mitsu MH8111 CAN",
    .protocols = kProtocols,
    .mcu = "MH8111",
    .family = FlashFamily::SubaruTcuCvtMitsuMh8111Can,
    .transport = TransportKind::CanIso15765,
    .read_region = kReadRegion,
    .write_region = kWriteRegion,
    .image_size = kImageSize,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_tcu_cvt_mitsu_mh8111_can_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                             std::string_view mcu_type,
                                                             std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruTcuCvtMitsuMh8111CanPlan{0x7e1, 0x7e9, 500000, false});
}
} // namespace fastecu::flash
