#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"

#include <format>
#include <utility>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
using enum ErrorKind;

constexpr std::string_view kProtocol = "sub_tcu_cvt_mitsu_mh8111_can";
constexpr std::string_view kMcu = "MH8111";

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

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol)
    {
        return fail(InvalidConfig, std::format("Unsupported Subaru TCU CVT Mitsu MH8111 CAN protocol: {}", protocol));
    }
    const int index = find_flash_device_index(mcu);
    if (index < 0)
    {
        return fail(InvalidConfig, std::format("Unknown MCU type: {}", mcu));
    }
    if (mcu != kMcu)
    {
        return fail(InvalidConfig, std::format("Protocol {} expects MCU {}; got {}", protocol, kMcu, mcu));
    }
    // Check block 0 (full-table sanity) and block 3 specifically -- block 3,
    // not block 0, is the block this family actually writes (see
    // kWriteRegion above).
    if (const flashdev_t& device = flashdevices[index];
        device.romsize != kImageSize || device.numblocks != 4 || device.fblocks[0].start != 0 ||
        device.fblocks[0].len != 0x40000 || device.fblocks[3].start != kWriteRegion.start ||
        device.fblocks[3].len != kWriteRegion.length)
    {
        return fail(InvalidConfig, "MH8111 flash geometry is invalid");
    }
    return {};
}
} // namespace

Status validate_subaru_tcu_cvt_mitsu_mh8111_can_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruTcuCvtMitsuMh8111Can || plan.transport() != TransportKind::CanIso15765)
    {
        return fail(InvalidConfig, "plan is not for Subaru TCU CVT Mitsu MH8111 CAN");
    }
    if (const auto *p = std::get_if<SubaruTcuCvtMitsuMh8111CanPlan>(&plan.family_plan());
        p == nullptr || p->request_id != 0x7e1 || p->response_id != 0x7e9 || p->bitrate != 500000 || p->extended_id)
    {
        return fail(InvalidConfig, "Mitsu MH8111 CAN wire parameters are invalid");
    }
    if (const MemoryRegion& expected_region = plan.operation() == FlashOperation::Read ? kReadRegion : kWriteRegion;
        plan.transfer_region().start != expected_region.start ||
        plan.transfer_region().length != expected_region.length)
    {
        return fail(InvalidConfig, "Mitsu MH8111 CAN transfer region is invalid");
    }
    if (plan.kernel())
    {
        return fail(InvalidConfig, "Mitsu MH8111 CAN plans are kernel-free");
    }
    if (plan.operation() == FlashOperation::TestWrite)
    {
        return fail(Unsupported, "test_write is not supported by this family");
    }
    if (plan.operation() == FlashOperation::Read && !plan.erase_regions().empty())
    {
        return fail(InvalidConfig, "read plans must not erase memory");
    }
    if (plan.operation() == FlashOperation::Write &&
        (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != kWriteRegion.start ||
         plan.erase_regions()[0].length != kWriteRegion.length))
    {
        return fail(InvalidConfig, "Mitsu MH8111 CAN erase region is invalid");
    }
    if (plan.operation() == FlashOperation::Write && (!plan.image().has_value() || plan.image()->size() != kImageSize))
    {
        return fail(InvalidConfig, "ROM file must be exactly 0x180000 bytes");
    }
    return {};
}

Result<FlashPlan> build_subaru_tcu_cvt_mitsu_mh8111_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                             std::string_view mcu_type,
                                                             std::optional<bytes::Bytes> image)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (operation == FlashOperation::TestWrite)
    {
        return fail(Unsupported, "test_write is not supported by this family");
    }
    if (operation == FlashOperation::Write && !image.has_value())
    {
        return fail(InvalidConfig, "Write plans must carry a ROM image");
    }
    if (operation == FlashOperation::Write && image->size() != kImageSize)
    {
        return fail(InvalidConfig,
                    std::format("ROM file must be exactly 0x180000 bytes; got 0x{:x} bytes", image->size()));
    }
    const MemoryRegion& region = operation == FlashOperation::Read ? kReadRegion : kWriteRegion;
    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruTcuCvtMitsuMh8111Can,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = region,
        .erase_regions = operation == FlashOperation::Write ? std::vector{kWriteRegion} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruTcuCvtMitsuMh8111CanPlan{0x7e1, 0x7e9, 500000, false},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_tcu_cvt_mitsu_mh8111_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
