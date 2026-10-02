#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"

#include <format>
#include <utility>

#include "src/backend/checksum/checksum_selection.h"
#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/protocol_catalog.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_operation_request.h"
#include "src/ui/desktop/checksum/checksum_correction_result.h"

namespace fastecu::ui
{
namespace
{

using config::ProtocolEntry;

// The checksum flag of a protocol without a checksum module.
constexpr std::string_view kNoChecksumModule = "n/a";

const config::ResolvedCarModel& selected_vehicle(const config::ConfigSession& config)
{
    // An initialized session only ever holds a valid row.
    return *config.selected_vehicle();
}

std::string selected_field(const config::ConfigSession& config, std::string ProtocolEntry::*field)
{
    return config::protocol_field_or_placeholder(selected_vehicle(config), field);
}

} // namespace

CalibrationOperationCoordinator::CalibrationOperationCoordinator(config::ConfigSession& config,
                                                                 calibration::RomSaveUseCase& saver,
                                                                 ICalibrationInteraction& interaction,
                                                                 CalibrationPresentationCallbacks callbacks)
    : config_(config), saver_(saver), interaction_(interaction), callbacks_(std::move(callbacks))
{
}

std::optional<PreparedWrite> CalibrationOperationCoordinator::prepare_write(calibration::CalibrationSession *session,
                                                                            std::string_view kernel_directory)
{
    if (session == nullptr)
    {
        interaction_.show_notice(CalibrationNotice::NoCalibrationToWrite);
        return std::nullopt;
    }

    bytes::Bytes image(session->rom().begin(), session->rom().end());
    if (selected_field(config_, &ProtocolEntry::checksum) == kNoChecksumModule &&
        !interaction_.confirm_write_without_checksum())
    {
        callbacks_.log(LogLevel::Debug, "Write canceled!");
        return std::nullopt;
    }
    refresh_write_metadata(*session, kernel_directory);

    // Read again: the refresh may have reselected the vehicle.
    if (selected_field(config_, &ProtocolEntry::checksum) != kNoChecksumModule)
    {
        correct_operation_image(*session, image);
    }
    return PreparedWrite{
        .image = std::move(image),
        .protocol = selected_vehicle(config_).protocol_name,
        .mcu = session->protocol().mcu_type,
        .kernel_path = session->protocol().kernel_path,
        .display_filename = session->source().display_name,
    };
}

// Legacy filled an empty flash method only when a definition left it empty --
// a definitionless ROM shows the " " placeholder -- and reselected the vehicle
// by that name before taking kernel and MCU from the selection. These values
// reach the ECU.
void CalibrationOperationCoordinator::refresh_write_metadata(calibration::CalibrationSession& session,
                                                             std::string_view kernel_directory)
{
    calibration::RomProtocolInfo protocol = session.protocol();
    if (session.definition() != nullptr && protocol.flash_method.empty())
    {
        protocol.flash_method = selected_vehicle(config_).protocol_name;
        session.set_protocol(protocol);
        callbacks_.log(LogLevel::Debug,
                       std::format("Update protocol info by selected ROM with FlashMethod: {}", protocol.flash_method));
        // The last matching row wins, as the legacy scan did; no match changes
        // nothing.
        if (config_.select_by_protocol_name(protocol.flash_method))
        {
            callbacks_.log(LogLevel::Debug, "Protocol info for selected ROM updated");
        }
        else
        {
            callbacks_.log(LogLevel::Debug, "Could not find protocol for selected ROM!");
        }
        callbacks_.protocol_description_changed(selected_field(config_, &ProtocolEntry::description));
    }
    protocol.kernel_path = flash::kernel_path(kernel_directory, selected_field(config_, &ProtocolEntry::kernel));
    protocol.kernel_start_address = selected_field(config_, &ProtocolEntry::kernel_addr);
    protocol.mcu_type = selected_field(config_, &ProtocolEntry::mcu);
    session.set_protocol(protocol);
}

// Corrected bytes replace only the operation image, never the editable
// session. An unknown MCU, a declined correction, or an outcome without
// corrected bytes leaves the image as it was.
void CalibrationOperationCoordinator::correct_operation_image(const calibration::CalibrationSession& session,
                                                              bytes::Bytes& image)
{
    const config::ResolvedCarModel& vehicle = selected_vehicle(config_);
    const checksum::ChecksumSelection selection{
        .make = vehicle.make,
        .checksum_flag = config::protocol_field_or_placeholder(vehicle, &ProtocolEntry::checksum),
        .flash_method = vehicle.protocol_name,
        .mcu_type = session.protocol().mcu_type,
        .rom_id = session.protocol().rom_id,
    };
    callbacks_.log(LogLevel::Debug, std::format("Protocol: {}", selection.flash_method));
    callbacks_.log(LogLevel::Debug, std::format("Make: {}", selection.make));
    callbacks_.log(LogLevel::Debug, std::format("Checksum: {}", selection.checksum_flag));
    if (const auto *device = flash::find_flash_device(selection.mcu_type); device != nullptr)
    {
        callbacks_.log(LogLevel::Debug,
                       std::format("ecuCalDef->McuType: {} {}", session.protocol().mcu_type,
                                   config::protocol_field_or_placeholder(vehicle, &ProtocolEntry::mcu)));
        callbacks_.log(LogLevel::Debug, std::format("Size: 0x{:x} -> 0x{:x}", image.size(), device->romsize));
    }
    const ChecksumCorrectionResult result =
        interaction_.correct_checksums(image, session.definition() != nullptr, selection);
    if (result.unknown_mcu_type)
    {
        callbacks_.log(LogLevel::Error, std::format("Unknown MCU type: {}", session.protocol().mcu_type));
        return;
    }
    if (result.canceled_due_to_missing_module)
    {
        callbacks_.log(LogLevel::Debug, "Checksum calculation canceled!");
    }
    if (result.corrected_rom_data.has_value())
    {
        image = *result.corrected_rom_data;
    }
}

} // namespace fastecu::ui
