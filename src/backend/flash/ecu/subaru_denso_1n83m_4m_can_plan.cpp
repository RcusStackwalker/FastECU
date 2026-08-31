#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"

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

constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_4m_can";
constexpr std::string_view kMcu = "N83M_4MB";
// fblocks_N83M_4MB[1], the window legacy read_memory hardcodes over its own
// arguments (lines 834-836) and the 0x34/0x35 setup PDUs spell out literally
// (lines 845-860, 886-901).
constexpr MemoryRegion kMainBlock{0x08FAC000, 0x003D3F00};
constexpr std::uint32_t kImageStart = 0x08F9C000; // fblocks[0].start
constexpr std::size_t kImageSize = 0x3E4000;      // fblocks[0..2] summed, and N83M_4MB's own romsize
constexpr std::uint32_t kLeadPad = 0x10000;
constexpr std::uint32_t kTailPad = 0x100;

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol)
    {
        return fail(InvalidConfig, std::format("Unsupported Subaru Denso 1N83M 4M CAN protocol: {}", protocol));
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
    // Unlike its N83M_1_5MB sibling, N83M_4MB's declared romsize (0x3E4000)
    // does equal its own fblocks sum, so -- as in SH72531 and SH72543d, whose
    // tables are likewise self-consistent -- it is checked here. Nothing on
    // either path consumes romsize (read_memory discards the length argument
    // derived from it at line 836); the check guards the table, not the
    // transfer.
    if (const flashdev_t& device = flashdevices[index];
        device.numblocks != 3 || device.romsize != kImageSize || device.fblocks[0].start != kImageStart ||
        device.fblocks[1].start != kMainBlock.start || device.fblocks[1].len != kMainBlock.length)
    {
        return fail(InvalidConfig, "N83M_4MB three-block flash geometry is invalid");
    }
    return {};
}
} // namespace

Status validate_subaru_denso_1n83m_4m_can_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruDenso1n83m_4mCan || plan.transport() != TransportKind::CanIso15765)
    {
        return fail(InvalidConfig, "plan is not for Subaru Denso 1N83M 4M CAN");
    }
    if (const auto *p = std::get_if<SubaruDenso1n83m_4mCanPlan>(&plan.family_plan());
        p == nullptr || p->request_id != 0x7e0 || p->response_id != 0x7e8 || p->bitrate != 500000 || p->extended_id ||
        p->lead_pad_len != kLeadPad || p->tail_pad_len != kTailPad)
    {
        return fail(InvalidConfig, "Denso 1N83M 4M CAN wire parameters are invalid");
    }
    if (plan.transfer_region().start != kMainBlock.start || plan.transfer_region().length != kMainBlock.length)
    {
        return fail(InvalidConfig, "Denso 1N83M 4M CAN transfer region is invalid");
    }
    if (plan.kernel())
    {
        return fail(InvalidConfig, "Denso 1N83M 4M CAN plans are kernel-free");
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
        (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != kMainBlock.start ||
         plan.erase_regions()[0].length != kMainBlock.length))
    {
        return fail(InvalidConfig, "Denso 1N83M 4M CAN erase region is invalid");
    }
    if (plan.operation() == FlashOperation::Write && (!plan.image().has_value() || plan.image()->size() != kImageSize))
    {
        return fail(InvalidConfig, "ROM file must be exactly 0x3E4000 bytes");
    }
    return {};
}

Result<FlashPlan> build_subaru_denso_1n83m_4m_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                       std::string_view mcu_type, std::optional<bytes::Bytes> image)
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
                    std::format("ROM file must be exactly 0x3E4000 bytes; got 0x{:x} bytes", image->size()));
    }
    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruDenso1n83m_4mCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = kMainBlock,
        .erase_regions = operation == FlashOperation::Write ? std::vector{kMainBlock} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruDenso1n83m_4mCanPlan{0x7e0, 0x7e8, 500000, false, kLeadPad, kTailPad},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_denso_1n83m_4m_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
