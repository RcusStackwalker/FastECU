#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <array>
#include <format>
#include <memory>
#include <ranges>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
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

const ColtVariant *FindVariant(std::string_view protocol_id)
{
    const auto it = std::ranges::find(kColtVariants, protocol_id, &ColtVariant::protocol_id);
    return it == kColtVariants.end() ? nullptr : std::to_address(it);
}

Result<const ColtVariant *> RequireVariant(std::string_view protocol_id)
{
    if (const ColtVariant *variant = FindVariant(protocol_id); variant != nullptr)
    {
        return variant;
    }

    return Fail(ErrorKind::kInvalidConfig,
                std::format("Unsupported Mitsubishi Colt M32R CAN protocol: {}", protocol_id));
}

} // namespace

Status ValidateMitsuColtM32rCanPlan(const FlashPlan& plan)
{
    const auto variant = RequireVariant(plan.TargetId());
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }
    if (plan.Family() != FlashFamily::kMitsuColtM32rCan)
    {
        return Fail(ErrorKind::kInvalidConfig, "plan is not for Mitsubishi Colt M32R CAN");
    }
    if (plan.McuName() != (*variant)->mcu)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("Protocol {} expects MCU {}; got {}", plan.TargetId(),
                                                           (*variant)->mcu, plan.McuName()));
    }
    if (const auto *family = std::get_if<MitsuColtM32rCanPlan>(&plan.FamilyPlan());
        family == nullptr || family->use_vendor_challenge != (*variant)->vendor)
    {
        return Fail(ErrorKind::kInvalidConfig, "Mitsubishi Colt authorization variant does not match protocol");
    }

    const bool read = plan.Operation() == FlashOperation::kRead;
    if (const MemoryRegion expected{read ? 0U : mitsu_colt_can::kUserspaceStart,
                                    (*variant)->capacity - (read ? 0U : mitsu_colt_can::kUserspaceStart)};
        plan.TransferRegion().start != expected.start || plan.TransferRegion().length != expected.length)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Transfer region does not match protocol capacity 0x{:x}", (*variant)->capacity));
    }
    if (const std::uint32_t rom_end = plan.TransferRegion().start + plan.TransferRegion().length;
        read ? plan.Image().has_value() : (!plan.Image().has_value() || plan.Image()->size() != rom_end))
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM image size does not match ROM extent 0x{:x}", rom_end));
    }
    return {};
}

Result<FlashPlan> BuildMitsuColtM32rCanPlan(FlashOperation operation, std::string_view protocol_name,
                                            std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    const auto variant = RequireVariant(protocol_name);
    if (!variant.has_value())
    {
        return std::unexpected(variant.error());
    }

    if (operation == FlashOperation::kTestWrite)
    {
        return Fail(ErrorKind::kUnsupported,
                    "test_write is not supported by this family; the built-in catalog declares "
                    "test_write=no and the legacy implementation performed only a "
                    "diagnostic-session handshake");
    }

    // Legacy: flash_ecu_mitsu_m32r_can_operation.cpp:24-29.
    if (FindFlashDeviceIndex(mcu_type) < 0)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("Unknown MCU type: {}", mcu_type));
    }
    if (mcu_type != (*variant)->mcu)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("Protocol {} expects MCU {}; got {}", protocol_name, (*variant)->mcu, mcu_type));
    }

    FlashPlanFields fields;
    fields.operation = operation;
    fields.family = FlashFamily::kMitsuColtM32rCan;
    fields.transport = TransportKind::kCanIso15765;
    fields.target_id = std::string(protocol_name);
    fields.mcu_name = std::string(mcu_type);
    fields.kernel = std::nullopt;

    fields.family_plan = MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = (*variant)->vendor,
        .session_id = mitsu_colt_can::kSessionBootload,
    };

    if (operation == FlashOperation::kRead)
    {
        fields.transfer_region = MemoryRegion{0, (*variant)->capacity};
    }
    else if (!image.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "Write plans must carry a ROM image");
    }
    else if (image->size() != (*variant)->capacity)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("ROM file must be exactly 0x{:x} bytes; got 0x{:x} bytes",
                                                           (*variant)->capacity, image->size()));
    }
    else
    {
        fields.transfer_region =
            MemoryRegion{mitsu_colt_can::kUserspaceStart, (*variant)->capacity - mitsu_colt_can::kUserspaceStart};
        fields.image = std::move(image);
        const std::string capacity_kib = std::to_string((*variant)->capacity / 1024);
        const std::string rom_end = std::format("0x{:x}", (*variant)->capacity);
        fields.confirmations = {
            ConfirmationSpec{ConfirmationSpec::Id::kEraseTrigger,
                             {{"capacity_kib", capacity_kib},
                              {"writable_start_hex", std::format("0x{:x}", mitsu_colt_can::kUserspaceStart)},
                              {"rom_end_hex", rom_end}}}};
        if ((*variant)->capacity == mitsu_colt_can::kFullRomSize)
        {
            fields.confirmations.push_back(
                ConfirmationSpec{ConfirmationSpec::Id::kTopRegionBootstrap,
                                 {{"top_region_start_hex", "0x60000"}, {"rom_end_hex", rom_end}}});
        }
    }

    auto plan = ValidateAndBuild(std::move(fields));
    if (!plan)
    {
        return std::unexpected(plan.error());
    }
    if (Status valid = ValidateMitsuColtM32rCanPlan(*plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return plan;
}

} // namespace fastecu::flash
