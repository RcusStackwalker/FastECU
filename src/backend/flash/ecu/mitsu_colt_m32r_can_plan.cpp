#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <format>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{

Result<MitsuColtProtocolOptions> parse_mitsu_colt_protocol(std::string_view protocol_name)
{
    if (protocol_name == "mitsu_ecu_m32r_can")
    {
        return MitsuColtProtocolOptions{.use_vendor_challenge = false, .rom_size = 0x60000};
    }
    if (protocol_name == "mitsu_ecu_m32r_can_vendor_ext")
    {
        return MitsuColtProtocolOptions{.use_vendor_challenge = true, .rom_size = 0x60000};
    }
    if (protocol_name == "mitsu_ecu_m32r_can_512kb")
    {
        return MitsuColtProtocolOptions{.use_vendor_challenge = false, .rom_size = 0x80000};
    }
    if (protocol_name == "mitsu_ecu_m32r_can_vendor_ext_512kb")
    {
        return MitsuColtProtocolOptions{.use_vendor_challenge = true, .rom_size = 0x80000};
    }

    return fail(ErrorKind::InvalidConfig,
                std::format("Unsupported Mitsubishi Colt M32R CAN protocol: {}", protocol_name));
}

Result<FlashPlan> build_mitsu_colt_m32r_can_plan(FlashOperation operation,
                                                 std::string_view protocol_name,
                                                 std::string_view mcu_type,
                                                 std::optional<bytes::Bytes> image)
{
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by this family; protocols.cfg declares "
                    "test_write=no and the legacy implementation performed only a "
                    "diagnostic-session handshake");
    }

    const auto options = parse_mitsu_colt_protocol(protocol_name);
    if (!options.has_value())
    {
        return std::unexpected(options.error());
    }

    // Legacy: flash_ecu_mitsu_m32r_can_operation.cpp:24-29.
    if (find_flash_device_index(mcu_type) < 0)
    {
        return fail(ErrorKind::InvalidConfig, std::format("Unknown MCU type: {}", mcu_type));
    }

    FlashPlanFields fields;
    fields.operation = operation;
    fields.family = FlashFamily::MitsuColtM32rCan;
    fields.transport = TransportKind::CanIso15765;
    fields.target_id = std::string(protocol_name);
    fields.mcu_name = std::string(mcu_type);
    fields.kernel = std::nullopt;

    if (operation == FlashOperation::Read)
    {
        fields.transfer_region = MemoryRegion{0, options->rom_size};
        fields.family_plan = MitsuColtM32rCanPlan{
            .request_id = 0x7e0,
            .response_id = 0x7e8,
            .bitrate = 500000,
            .extended_id = false,
            .use_vendor_challenge = options->use_vendor_challenge,
            .rom_size = options->rom_size,
            .session_id = MitsuColtCan::kSessionBootload,
        };
        return validate_and_build(std::move(fields));
    }

    if (!image.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Write plans must carry a ROM image");
    }
    if (image->size() != options->rom_size)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("ROM file must be exactly 0x{:x} bytes; got 0x{:x} bytes",
                                options->rom_size, image->size()));
    }

    // The complete file remains aligned to absolute ROM addresses. The
    // protected bootloader prefix is deliberately omitted from the aggregate
    // transfer region and is never read, compared, erased, or written.
    fields.transfer_region = MemoryRegion{MitsuColtCan::kUserspaceStart,
                                          options->rom_size - MitsuColtCan::kUserspaceStart};
    fields.image = std::move(image);
    fields.family_plan = MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = options->use_vendor_challenge,
        .rom_size = options->rom_size,
        .session_id = MitsuColtCan::kSessionBootload,
    };
    fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::EraseTrigger, {}}};
    if (options->rom_size == MitsuColtCan::kFullRomSize)
    {
        fields.confirmations.push_back(
            ConfirmationSpec{ConfirmationSpec::Id::TopRegionBootstrap, {}});
    }

    return validate_and_build(std::move(fields));
}

} // namespace fastecu::flash
