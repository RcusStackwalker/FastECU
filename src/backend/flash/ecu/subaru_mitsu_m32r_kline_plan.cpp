#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"

#include <array>
#include <format>
#include <utility>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{
constexpr std::string_view kProtocol = "sub_ecu_mitsu_m32r_kline";
constexpr std::string_view kMcu = "M32R_512KB_4blocks";
constexpr MemoryRegion kUserspace{0x8000, 0x78000};

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    using enum ErrorKind;

    if (protocol != kProtocol)
    {
        return fail(InvalidConfig, std::format("Unsupported Subaru Mitsubishi M32R K-Line protocol: {}", protocol));
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
    const flashdev_t& device = flashdevices[index];
    constexpr std::array<flashblock, 4> expected{{{0, 0x4000}, {0x4000, 0x2000}, {0x6000, 0x2000}, {0x8000, 0x78000}}};
    if (device.romsize != 0x80000 || device.numblocks != expected.size())
    {
        return fail(InvalidConfig, "M32R four-block flash geometry is invalid");
    }
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        if (device.fblocks[i].start != expected[i].start || device.fblocks[i].len != expected[i].len)
        {
            return fail(InvalidConfig, "M32R four-block flash geometry is invalid");
        }
    }
    return {};
}
} // namespace

Status validate_subaru_mitsu_m32r_kline_plan(const FlashPlan& plan)
{
    using enum ErrorKind;

    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruMitsuM32rKline || plan.transport() != TransportKind::Kline)
    {
        return fail(InvalidConfig, "plan is not for Subaru Mitsubishi M32R K-Line");
    }
    if (const auto *family = std::get_if<SubaruMitsuM32rKlinePlan>(&plan.family_plan());
        family == nullptr || family->tester_id != 0xf0 || family->target_id != 0x10 || family->initial_baud != 4800 ||
        family->flash_baud != 15625 || family->chunk_size != 128 || family->unread_prefix_fill != 0xff)
    {
        return fail(InvalidConfig, "M32R K-Line wire parameters are invalid");
    }
    if (plan.transfer_region().start != kUserspace.start || plan.transfer_region().length != kUserspace.length)
    {
        return fail(InvalidConfig, "M32R K-Line transfer region is invalid");
    }
    if (plan.kernel())
    {
        return fail(InvalidConfig, "M32R K-Line plans are kernel-free");
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
        (plan.erase_regions().size() != 1 || plan.erase_regions()[0].start != kUserspace.start ||
         plan.erase_regions()[0].length != kUserspace.length))
    {
        return fail(InvalidConfig, "M32R K-Line erase region is invalid");
    }
    if (plan.operation() == FlashOperation::Write && (!plan.image().has_value() || plan.image()->size() != 0x80000))
    {
        return fail(InvalidConfig, "ROM file must be exactly 0x80000 bytes");
    }
    return {};
}

Result<FlashPlan> build_subaru_mitsu_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
                                                     std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    if (auto valid = validate_identity(protocol_name, mcu_type); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by this family; protocols.cfg declares test_write=no");
    }
    if (operation == FlashOperation::Write && !image.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Write plans must carry a ROM image");
    }
    if (operation == FlashOperation::Write && image->size() != 0x80000)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("ROM file must be exactly 0x80000 bytes; got 0x{:x} bytes", image->size()));
    }

    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruMitsuM32rKline,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = kUserspace,
        .erase_regions = operation == FlashOperation::Write ? std::vector{kUserspace} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruMitsuM32rKlinePlan{0xf0, 0x10, 4800, 15625, 128, 0xff},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_mitsu_m32r_kline_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
