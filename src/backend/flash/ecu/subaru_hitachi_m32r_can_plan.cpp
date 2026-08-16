#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"

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

constexpr std::string_view kProtocol = "sub_ecu_hitachi_m32r_can";
constexpr std::string_view kMcu = "M32R_512KB_1block";
constexpr MemoryRegion kRom{0, 0x80000};

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (protocol != kProtocol)
    {
        return fail(InvalidConfig, std::format("Unsupported Subaru Hitachi M32R CAN protocol: {}", protocol));
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
    if (const flashdev_t& device = flashdevices[index]; device.romsize != kRom.length || device.numblocks != 1 ||
                                                        device.fblocks[0].start != kRom.start ||
                                                        device.fblocks[0].len != kRom.length)
    {
        return fail(InvalidConfig, "M32R one-block flash geometry is invalid");
    }
    return {};
}
} // namespace

Status validate_subaru_hitachi_m32r_can_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruHitachiM32rCan || plan.transport() != TransportKind::CanIso15765)
    {
        return fail(InvalidConfig, "plan is not for Subaru Hitachi M32R CAN");
    }
    if (const auto *p = std::get_if<SubaruHitachiM32rCanPlan>(&plan.family_plan());
        p == nullptr || p->request_id != 0x7e0 || p->response_id != 0x7e8 || p->bitrate != 500000 || p->extended_id)
    {
        return fail(InvalidConfig, "Hitachi M32R CAN wire parameters are invalid");
    }
    if (plan.transfer_region().start != kRom.start || plan.transfer_region().length != kRom.length)
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
        (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != 0 ||
         plan.erase_regions()[0].length != kRom.length))
    {
        return fail(InvalidConfig, "Hitachi M32R CAN erase region is invalid");
    }
    if (plan.operation() == FlashOperation::Write && (!plan.image().has_value() || plan.image()->size() != kRom.length))
    {
        return fail(InvalidConfig, "ROM file must be exactly 0x80000 bytes");
    }
    return {};
}

Result<FlashPlan> build_subaru_hitachi_m32r_can_plan(FlashOperation operation, std::string_view protocol_name,
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
    if (operation == FlashOperation::Write && image->size() != kRom.length)
    {
        return fail(InvalidConfig,
                    std::format("ROM file must be exactly 0x80000 bytes; got 0x{:x} bytes", image->size()));
    }
    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruHitachiM32rCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = kRom,
        .erase_regions = operation == FlashOperation::Write ? std::vector{kRom} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiM32rCanPlan{0x7e0, 0x7e8, 500000, false},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_hitachi_m32r_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
