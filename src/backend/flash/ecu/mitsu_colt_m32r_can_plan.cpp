#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <array>
#include <format>
#include <memory>
#include <ranges>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

struct ColtVariant
{
    std::string_view protocol_id;
    std::string_view mcu;
    bool vendor;
    std::uint32_t capacity;
};

constexpr std::array<ColtVariant, 4> kColtVariants{{
    {"mitsu_ecu_m32r_can", "M32R_384KB_1block", false, 0x60000},
    {"mitsu_ecu_m32r_can_vendor_ext", "M32R_384KB_1block", true, 0x60000},
    {"mitsu_ecu_m32r_can_512kb", "M32R_512KB_1block", false, 0x80000},
    {"mitsu_ecu_m32r_can_vendor_ext_512kb", "M32R_512KB_1block", true, 0x80000},
}};

const ColtVariant *find_variant(std::string_view protocol_id)
{
    const auto it = std::ranges::find(kColtVariants, protocol_id, &ColtVariant::protocol_id);
    return it == kColtVariants.end() ? nullptr : std::to_address(it);
}

Result<const ColtVariant *> require_variant(std::string_view protocol_id)
{
    if (const ColtVariant *variant = find_variant(protocol_id); variant != nullptr)
    {
        return variant;
    }

    return fail(ErrorKind::InvalidConfig,
                std::format("Unsupported Mitsubishi Colt M32R CAN protocol: {}", protocol_id));
}

} // namespace

Status validate_mitsu_colt_m32r_can_plan(const FlashPlan& plan)
{
    const auto variant = require_variant(plan.target_id());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (plan.family() != FlashFamily::MitsuColtM32rCan)
    {
        return fail(ErrorKind::InvalidConfig, "plan is not for Mitsubishi Colt M32R CAN");
    }
    if (plan.mcu_name() != (*variant)->mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Protocol {} expects MCU {}; got {}", plan.target_id(),
                                (*variant)->mcu, plan.mcu_name()));
    }
    if (const auto *family = std::get_if<MitsuColtM32rCanPlan>(&plan.family_plan());
        family == nullptr || family->use_vendor_challenge != (*variant)->vendor)
    {
        return fail(ErrorKind::InvalidConfig,
                    "Mitsubishi Colt authorization variant does not match protocol");
    }

    const bool read = plan.operation() == FlashOperation::Read;
    if (const MemoryRegion expected{read ? 0u : MitsuColtCan::kUserspaceStart,
                                    (*variant)->capacity -
                                        (read ? 0u : MitsuColtCan::kUserspaceStart)};
        plan.transfer_region().start != expected.start ||
        plan.transfer_region().length != expected.length)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Transfer region does not match protocol capacity 0x{:x}",
                                (*variant)->capacity));
    }
    if (const std::uint32_t rom_end =
            plan.transfer_region().start + plan.transfer_region().length;
        read ? plan.image().has_value()
             : (!plan.image().has_value() || plan.image()->size() != rom_end))
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("ROM image size does not match ROM extent 0x{:x}", rom_end));
    }
    return {};
}

Result<FlashPlan> build_mitsu_colt_m32r_can_plan(FlashOperation operation,
                                                 std::string_view protocol_name,
                                                 std::string_view mcu_type,
                                                 std::optional<bytes::Bytes> image)
{
    const auto variant = require_variant(protocol_name);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }

    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by this family; protocols.cfg declares "
                    "test_write=no and the legacy implementation performed only a "
                    "diagnostic-session handshake");
    }

    // Legacy: flash_ecu_mitsu_m32r_can_operation.cpp:24-29.
    if (find_flash_device_index(mcu_type) < 0)
    {
        return fail(ErrorKind::InvalidConfig, std::format("Unknown MCU type: {}", mcu_type));
    }
    if (mcu_type != (*variant)->mcu)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("Protocol {} expects MCU {}; got {}", protocol_name,
                                (*variant)->mcu, mcu_type));
    }

    FlashPlanFields fields;
    fields.operation = operation;
    fields.family = FlashFamily::MitsuColtM32rCan;
    fields.transport = TransportKind::CanIso15765;
    fields.target_id = std::string(protocol_name);
    fields.mcu_name = std::string(mcu_type);
    fields.kernel = std::nullopt;

    fields.family_plan = MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = (*variant)->vendor,
        .session_id = MitsuColtCan::kSessionBootload,
    };

    if (operation == FlashOperation::Read)
    {
        fields.transfer_region = MemoryRegion{0, (*variant)->capacity};
    }
    else if (!image.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Write plans must carry a ROM image");
    }
    else if (image->size() != (*variant)->capacity)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("ROM file must be exactly 0x{:x} bytes; got 0x{:x} bytes",
                                (*variant)->capacity, image->size()));
    }
    else
    {
        fields.transfer_region = MemoryRegion{MitsuColtCan::kUserspaceStart,
                                              (*variant)->capacity -
                                                  MitsuColtCan::kUserspaceStart};
        fields.image = std::move(image);
        const std::string capacity_kib = std::to_string((*variant)->capacity / 1024);
        const std::string rom_end = std::format("0x{:x}", (*variant)->capacity);
        fields.confirmations = {ConfirmationSpec{
            ConfirmationSpec::Id::EraseTrigger,
            {{"capacity_kib", capacity_kib},
             {"writable_start_hex", std::format("0x{:x}", MitsuColtCan::kUserspaceStart)},
             {"rom_end_hex", rom_end}}}};
        if ((*variant)->capacity == MitsuColtCan::kFullRomSize)
        {
            fields.confirmations.push_back(ConfirmationSpec{
                ConfirmationSpec::Id::TopRegionBootstrap,
                {{"top_region_start_hex", "0x60000"}, {"rom_end_hex", rom_end}}});
        }
    }

    auto plan = validate_and_build(std::move(fields));
    if (!plan)
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = validate_mitsu_colt_m32r_can_plan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
