#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <format>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{

Result<FlashPlan> build_mitsu_colt_m32r_can_plan(FlashOperation operation,
                                                 std::string_view protocol_name,
                                                 std::string_view mcu_type,
                                                 bool use_vendor_challenge,
                                                 std::optional<bytes::Bytes> image)
{
    if (operation == FlashOperation::TestWrite)
    {
        return fail(ErrorKind::Unsupported,
                    "test_write is not supported by this family; protocols.cfg declares "
                    "test_write=no and the legacy implementation performed only a "
                    "diagnostic-session handshake");
    }

    // Legacy: flash_ecu_mitsu_m32r_can_operation.cpp:24-29.
    const int mcu_index = find_flash_device_index(mcu_type);
    if (mcu_index < 0)
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
        // Legacy: flash_ecu_mitsu_m32r_can_operation.cpp:44-45.
        fields.transfer_region = MemoryRegion{flashdevices[mcu_index].fblocks[0].start,
                                              flashdevices[mcu_index].fblocks[0].len};
        fields.family_plan = MitsuColtM32rCanPlan{
            .request_id = 0x7e0,
            .response_id = 0x7e8,
            .bitrate = 500000,
            .extended_id = false,
            .use_vendor_challenge = use_vendor_challenge,
            .session_id = MitsuColtCan::kSessionBootload,
        };
        return validate_and_build(std::move(fields));
    }

    if (!image.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "Write plans must carry a ROM image");
    }
    if (image->size() != MitsuColtCan::kFullRomSize)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("ROM file must be exactly 0x{:x} bytes (512 KiB); got "
                                "0x{:x} bytes",
                                MitsuColtCan::kFullRomSize, image->size()));
    }

    // The complete file remains aligned to absolute ROM addresses. The
    // protected bootloader prefix is deliberately omitted from the aggregate
    // transfer region and is never read, compared, erased, or written.
    fields.transfer_region = MemoryRegion{MitsuColtCan::kUserspaceStart,
                                          MitsuColtCan::kWritableLength};
    fields.image = std::move(image);
    fields.family_plan = MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = use_vendor_challenge,
        .session_id = MitsuColtCan::kSessionBootload,
    };
    fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::EraseTrigger, {}},
                            ConfirmationSpec{ConfirmationSpec::Id::TopRegionBootstrap, {}}};

    return validate_and_build(std::move(fields));
}

} // namespace fastecu::flash
