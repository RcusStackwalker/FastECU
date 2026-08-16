#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"

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

constexpr std::string_view kProtocol = "sub_tcu_cvt_mitsu_mh8104_can";
constexpr std::string_view kMcu = "MH8104";

// Legacy read_mem hardcodes start_addr = 0x8000, length = 0x78000
// unconditionally (lines 364-366, "hack for testing" -- overwriting
// whatever the caller passed), the same hardcoding shape as the sibling
// MH8111 family.
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
// fdt->fblocks[3].len == 0x80000, even though the flashed window is the
// same size as the image's own upper 0x78000 bytes.
constexpr std::uint32_t kImageSize = 0x80000;

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol)
    {
        return fail(InvalidConfig,
                    std::format("Unsupported Subaru TCU CVT Mitsu MH8104 CAN protocol: {}",
                                protocol));
    }
    const int index = find_flash_device_index(mcu);
    if (index < 0)
    {
        return fail(InvalidConfig, std::format("Unknown MCU type: {}", mcu));
    }
    if (mcu != kMcu)
    {
        return fail(InvalidConfig,
                    std::format("Protocol {} expects MCU {}; got {}", protocol, kMcu, mcu));
    }
    // Check block 0 (full-table sanity) and block 3 specifically -- block 3
    // is both this family's read window AND the block it writes.
    if (const flashdev_t& device = flashdevices[index];
        device.romsize != kImageSize || device.numblocks != 4 || device.fblocks[0].start != 0 ||
        device.fblocks[0].len != 0x4000 || device.fblocks[3].start != kWriteRegion.start ||
        device.fblocks[3].len != kWriteRegion.length)
    {
        return fail(InvalidConfig, "MH8104 flash geometry is invalid");
    }
    return {};
}
} // namespace

Status validate_subaru_tcu_cvt_mitsu_mh8104_can_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruTcuCvtMitsuMh8104Can ||
        plan.transport() != TransportKind::CanIso15765)
    {
        return fail(InvalidConfig, "plan is not for Subaru TCU CVT Mitsu MH8104 CAN");
    }
    if (const auto *p = std::get_if<SubaruTcuCvtMitsuMh8104CanPlan>(&plan.family_plan());
        p == nullptr || p->request_id != 0x7e1 || p->response_id != 0x7e9 ||
        p->bitrate != 500000 || p->extended_id)
    {
        return fail(InvalidConfig, "Mitsu MH8104 CAN wire parameters are invalid");
    }
    // Read and write share the same region for this family -- both check
    // against the same constant.
    if (const MemoryRegion& expected_region =
            plan.operation() == FlashOperation::Read ? kReadRegion : kWriteRegion;
        plan.transfer_region().start != expected_region.start ||
        plan.transfer_region().length != expected_region.length)
    {
        return fail(InvalidConfig, "Mitsu MH8104 CAN transfer region is invalid");
    }
    if (plan.kernel())
    {
        return fail(InvalidConfig, "Mitsu MH8104 CAN plans are kernel-free");
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
        return fail(InvalidConfig, "Mitsu MH8104 CAN erase region is invalid");
    }
    if (plan.operation() == FlashOperation::Write &&
        (!plan.image().has_value() || plan.image()->size() != kImageSize))
    {
        return fail(InvalidConfig, "ROM file must be exactly 0x80000 bytes");
    }
    return {};
}

Result<FlashPlan> build_subaru_tcu_cvt_mitsu_mh8104_can_plan(FlashOperation operation,
                                                             std::string_view protocol_name,
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
                    std::format("ROM file must be exactly 0x80000 bytes; got 0x{:x} bytes",
                                image->size()));
    }
    const MemoryRegion& region = operation == FlashOperation::Read ? kReadRegion : kWriteRegion;
    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruTcuCvtMitsuMh8104Can,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = region,
        .erase_regions = operation == FlashOperation::Write ? std::vector{kWriteRegion}
                                                            : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruTcuCvtMitsuMh8104CanPlan{0x7e1, 0x7e9, 500000, false},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_tcu_cvt_mitsu_mh8104_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
