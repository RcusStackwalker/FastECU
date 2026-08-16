#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"

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

constexpr std::string_view kNormal = "sub_ecu_hitachi_m32r_kline";
constexpr std::string_view kRecovery = "sub_ecu_hitachi_m32r_kline_recovery";
constexpr std::string_view kMcu = "M32R_512KB_1block";
constexpr MemoryRegion kRom{0, 0x80000};

Result<HitachiM32rKlineSessionMode> mode_for(std::string_view protocol)
{
    if (protocol == kNormal)
    {
        return HitachiM32rKlineSessionMode::Normal;
    }
    if (protocol == kRecovery)
    {
        return HitachiM32rKlineSessionMode::Recovery;
    }
    return fail(InvalidConfig, std::format("Unsupported Subaru Hitachi M32R K-Line protocol: {}", protocol));
}

Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    if (!mode_for(protocol).has_value())
    {
        return fail(InvalidConfig, std::format("Unsupported Subaru Hitachi M32R K-Line protocol: {}", protocol));
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

Status validate_subaru_hitachi_m32r_kline_plan(const FlashPlan& plan)
{
    using enum ErrorKind;
    if (auto valid = validate_identity(plan.target_id(), plan.mcu_name()); !valid.has_value())
    {
        return valid;
    }
    if (plan.family() != FlashFamily::SubaruHitachiM32rKline || plan.transport() != TransportKind::Kline)
    {
        return fail(InvalidConfig, "plan is not for Subaru Hitachi M32R K-Line");
    }
    const auto *p = std::get_if<SubaruHitachiM32rKlinePlan>(&plan.family_plan());
    if (auto expected_mode = mode_for(plan.target_id());
        p == nullptr || p->session_mode != *expected_mode || p->tester_id != 0xf0 || p->target_id != 0x10 ||
        p->initial_baud != 4800 || p->write_baud != 15625 || p->read_baud != 38400 || p->chunk_size != 128 ||
        p->read_address_bias != 0x100000)
    {
        return fail(InvalidConfig, "Hitachi M32R K-Line wire parameters are invalid");
    }
    if (plan.transfer_region().start != kRom.start || plan.transfer_region().length != kRom.length)
    {
        return fail(InvalidConfig, "Hitachi M32R K-Line transfer region is invalid");
    }
    if (plan.kernel())
    {
        return fail(InvalidConfig, "Hitachi M32R K-Line plans are kernel-free");
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
        return fail(InvalidConfig, "Hitachi M32R K-Line erase region is invalid");
    }
    if (plan.operation() == FlashOperation::Write && (!plan.image().has_value() || plan.image()->size() != kRom.length))
    {
        return fail(InvalidConfig, "ROM file must be exactly 0x80000 bytes");
    }
    return {};
}

Result<FlashPlan> build_subaru_hitachi_m32r_kline_plan(FlashOperation operation, std::string_view protocol_name,
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
    const auto mode = *mode_for(protocol_name);
    FlashPlanFields fields{
        .operation = operation,
        .family = FlashFamily::SubaruHitachiM32rKline,
        .transport = TransportKind::Kline,
        .target_id = std::string(protocol_name),
        .mcu_name = std::string(mcu_type),
        .transfer_region = kRom,
        .erase_regions = operation == FlashOperation::Write ? std::vector{kRom} : std::vector<MemoryRegion>{},
        .image = operation == FlashOperation::Write ? std::move(image) : std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiM32rKlinePlan{mode, 0xf0, 0x10, 4800, 15625, 38400, 128, 0x100000},
    };
    auto plan = validate_and_build(std::move(fields));
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    if (auto valid = validate_subaru_hitachi_m32r_kline_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}
} // namespace fastecu::flash
