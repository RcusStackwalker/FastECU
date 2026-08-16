#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"

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

constexpr std::string_view kProtocol = "sub_tcu_cvt_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB";
// Legacy read_mem computes start_addr = 0 - 0x00100000, which underflows
// uint32_t to 0xFFF00000 and bypasses read_mem's own "< 0x8000" floor clamp
// entirely (0xFFF00000 is not less than 0x8000) -- that path never executed
// on real hardware (execute() called hack_words(), never read_mem()). This
// plan targets the clamp's evident intent instead of the never-observed
// underflowed address; see the design's dead-code decision and Task 3's
// second deliberate divergence. Read and write share the same window: the
// same 8 flash blocks (index 3-10 of M32R_512KB's 11) are both dumped and
// reflashed.
constexpr MemoryRegion kReadRegion{0x8000, 0x78000};
constexpr MemoryRegion kWriteRegion{0x8000, 0x78000};
// Legacy write_mem loads/writes the full, unclamped ROM image
// (ecuCalDef->FullRomData, romsize bytes) even though only kWriteRegion of
// it is ever transmitted -- the same full size read_mem's 0x8000-zero-pad +
// dumped-payload result produces.
constexpr std::uint32_t kImageSize = 0x80000;

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol)
    {
        return fail(InvalidConfig,
                    std::format("Unsupported Subaru TCU CVT Hitachi M32R CAN protocol: {}",
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
    // Spot-check block 0 only, matching subaru_hitachi_m32r_kline_plan.cpp's
    // precedent -- not every one of M32R_512KB's 11 blocks.
    if (const flashdev_t& device = flashdevices[index];
        device.romsize != kImageSize || device.numblocks != 11 || device.fblocks[0].start != 0 ||
        device.fblocks[0].len != 0x4000)
    {
        return fail(InvalidConfig, "M32R_512KB flash geometry is invalid");
    }
    return {};
}
} // namespace

Status validate_subaru_tcu_cvt_hitachi_m32r_can_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruTcuCvtHitachiM32rCan ||
        plan.transport() != TransportKind::CanIso15765)
    {
        return fail(InvalidConfig, "plan is not for Subaru TCU CVT Hitachi M32R CAN");
    }
    if (const auto *p = std::get_if<SubaruTcuCvtHitachiM32rCanPlan>(&plan.family_plan());
        p == nullptr || p->request_id != 0x7e1 || p->response_id != 0x7e9 ||
        p->bitrate != 500000 || p->extended_id)
    {
        return fail(InvalidConfig, "Hitachi M32R CAN wire parameters are invalid");
    }
    if (const MemoryRegion& expected_region =
            plan.operation() == FlashOperation::Read ? kReadRegion : kWriteRegion;
        plan.transfer_region().start != expected_region.start ||
        plan.transfer_region().length != expected_region.length)
    {
        return fail(InvalidConfig, "Hitachi M32R CAN transfer region is invalid");
    }
    if (plan.kernel())
    {
        return fail(InvalidConfig, "Hitachi M32R CAN plans are kernel-free");
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
        return fail(InvalidConfig, "Hitachi M32R CAN erase region is invalid");
    }
    if (plan.operation() == FlashOperation::Write &&
        (!plan.image().has_value() || plan.image()->size() != kImageSize))
    {
        return fail(InvalidConfig, "ROM file must be exactly 0x80000 bytes");
    }
    return {};
}

Result<FlashPlan> build_subaru_tcu_cvt_hitachi_m32r_can_plan(FlashOperation operation,
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
        .family = FlashFamily::SubaruTcuCvtHitachiM32rCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = region,
        .erase_regions = operation == FlashOperation::Write ? std::vector{kWriteRegion}
                                                            : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruTcuCvtHitachiM32rCanPlan{0x7e1, 0x7e9, 500000, false},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_tcu_cvt_hitachi_m32r_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
