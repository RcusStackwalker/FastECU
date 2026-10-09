#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"

#include <format>
#include <utility>

#include "src/backend/checksum/checksum_selection.h"
#include "src/backend/config/catalog.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_operation_request.h"
#include "src/ui/desktop/checksum/checksum_correction_result.h"

namespace fastecu::ui
{
namespace
{

// The suffix Save As ensures, compared case-sensitively.
constexpr std::string_view kCalibrationSuffix = ".bin";

const config::VehicleSpec& selected_vehicle(const config::ConfigSession& config)
{
    // The desktop startup selection gate ensures a valid vehicle before this coordinator is used.
    return *config.selected_vehicle();
}

const config::ProtocolSpec& selected_protocol(const config::ConfigSession& config)
{
    return *selected_vehicle(config).protocol;
}

// Legacy Save As normalization: drop one trailing dot, then append ".bin"
// unless the name already ends in lowercase ".bin". Nothing else changes.
std::string with_calibration_suffix(std::string path)
{
    if (path.ends_with('.'))
    {
        path.pop_back();
    }
    if (!path.ends_with(kCalibrationSuffix))
    {
        path.append(kCalibrationSuffix);
    }
    return path;
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
                                                                            std::string_view kernelDirectory)
{
    if (session == nullptr)
    {
        interaction_.show_notice(CalibrationNotice::kNoCalibrationToWrite);
        return std::nullopt;
    }

    bytes::Bytes image(session->rom().begin(), session->rom().end());
    if (selected_protocol(config_).checksum == config::ChecksumSupport::kMissing &&
        !interaction_.confirm_write_without_checksum())
    {
        callbacks_.log(LogLevel::kDebug, "Write canceled!");
        return std::nullopt;
    }
    refresh_write_metadata(*session, kernelDirectory);

    // Read again: the refresh may have reselected the vehicle.
    if (selected_protocol(config_).checksum != config::ChecksumSupport::kMissing)
    {
        correct_operation_image(*session, image);
    }
    return PreparedWrite{
        .image = std::move(image),
        .protocol = selected_protocol(config_),
        .kernel_path = session->protocol().kernel_path,
        .display_filename = session->source().display_name,
    };
}

SaveOutcome CalibrationOperationCoordinator::save(calibration::CalibrationSession *session, SaveMode mode)
{
    if (session == nullptr)
    {
        interaction_.show_notice(CalibrationNotice::kNoCalibrationToSave);
        return SaveOutcome::kNoSelection;
    }
    if (mode == SaveMode::kSaveAs)
    {
        callbacks_.log(LogLevel::kDebug, "Save as: Check selected ROM number");
    }
    bytes::Bytes image(session->rom().begin(), session->rom().end());
    correct_operation_image(*session, image);

    const std::optional<std::string> target =
        mode == SaveMode::kSave ? std::optional{session->source().path} : choose_save_as_path(*session);
    if (!target.has_value())
    {
        return SaveOutcome::kCancelled;
    }
    // RomSaveUseCase logs and notices a repository failure itself; the
    // operator gets no second notice from here.
    if (!saver_.save(*session, *target, image).has_value())
    {
        callbacks_.log(LogLevel::kError, std::format("Calibration file not saved: {}", *target));
        return SaveOutcome::kFailed;
    }
    callbacks_.log(LogLevel::kDebug, std::format("ecuCalDef->FileName: {}", session->source().display_name));
    callbacks_.log(LogLevel::kDebug, std::format("ecuCalDef->FullFileName: {}", session->source().path));
    return SaveOutcome::kSaved;
}

// Legacy filled an empty flash method only when a definition left it empty --
// a definitionless ROM shows the " " placeholder -- and reselected the vehicle
// by that name before taking kernel and MCU from the selection. These values
// reach the ECU.
void CalibrationOperationCoordinator::refresh_write_metadata(calibration::CalibrationSession& session,
                                                             std::string_view kernelDirectory)
{
    calibration::RomProtocolInfo protocol = session.protocol();
    if (session.definition() != nullptr && protocol.flash_method.empty())
    {
        protocol.flash_method = std::string(selected_protocol(config_).name);
        session.set_protocol(protocol);
        // Mirrored in MainWindow::update_protocol_info; keep the two in sync.
        callbacks_.log(LogLevel::kDebug,
                       std::format("Update protocol info by selected ROM with FlashMethod: {}", protocol.flash_method));
        // The last matching row wins, as the legacy scan did; no match changes
        // nothing.
        if (config_.select_by_protocol_name(protocol.flash_method))
        {
            callbacks_.log(LogLevel::kDebug, "Protocol info for selected ROM updated");
        }
        else
        {
            callbacks_.log(LogLevel::kDebug, "Could not find protocol for selected ROM!");
        }
        callbacks_.protocol_description_changed(selected_protocol(config_).description);
    }
    protocol.kernel_path = flash::kernel_path(kernelDirectory, selected_protocol(config_).kernel);
    protocol.kernel_start_address = config::kernel_load_address_text(selected_protocol(config_));
    protocol.mcu_type = std::string(selected_protocol(config_).mcu);
    session.set_protocol(protocol);
}

// Corrected bytes replace only the operation image, never the editable
// session. An unknown MCU, a declined correction, or an outcome without
// corrected bytes leaves the image as it was.
void CalibrationOperationCoordinator::correct_operation_image(const calibration::CalibrationSession& session,
                                                              bytes::Bytes& image)
{
    const config::VehicleSpec& vehicle = selected_vehicle(config_);
    const checksum::ChecksumSelection selection{
        .make = std::string(vehicle.make),
        .checksum_flag = std::string(config::checksum_flag(vehicle.protocol->checksum)),
        .flash_method = std::string(vehicle.protocol->name),
        .mcu_type = session.protocol().mcu_type,
        .rom_id = session.protocol().rom_id,
    };
    callbacks_.log(LogLevel::kDebug, std::format("Protocol: {}", selection.flash_method));
    callbacks_.log(LogLevel::kDebug, std::format("Make: {}", selection.make));
    callbacks_.log(LogLevel::kDebug, std::format("Checksum: {}", selection.checksum_flag));
    if (const auto *device = flash::find_flash_device(selection.mcu_type); device != nullptr)
    {
        callbacks_.log(LogLevel::kDebug,
                       std::format("ecuCalDef->McuType: {} {}", session.protocol().mcu_type, vehicle.protocol->mcu));
        callbacks_.log(LogLevel::kDebug, std::format("Size: 0x{:x} -> 0x{:x}", image.size(), device->romsize));
    }
    const ChecksumCorrectionResult result =
        interaction_.correct_checksums(image, session.definition() != nullptr, selection);
    if (result.unknown_mcu_type)
    {
        callbacks_.log(LogLevel::kError, std::format("Unknown MCU type: {}", session.protocol().mcu_type));
        return;
    }
    if (result.canceled_due_to_missing_module)
    {
        callbacks_.log(LogLevel::kDebug, "Checksum calculation canceled!");
    }
    if (result.corrected_rom_data.has_value())
    {
        image = *result.corrected_rom_data;
    }
}

// The picker opens on the effective calibration directory joined with the
// session's file name, as legacy did. A dismissed picker or an empty choice
// shows the missing-filename notice and returns nullopt.
std::optional<std::string>
CalibrationOperationCoordinator::choose_save_as_path(const calibration::CalibrationSession& session)
{
    callbacks_.log(LogLevel::kDebug, "Save as: Check if OEM ECU file");
    const std::optional<std::string> chosen = interaction_.choose_save_path(
        config_.effective_paths().calibration_files_directory + session.source().display_name);
    if (!chosen.has_value() || chosen->empty())
    {
        interaction_.show_notice(CalibrationNotice::kNoSaveFilename);
        return std::nullopt;
    }
    return with_calibration_suffix(*chosen);
}

} // namespace fastecu::ui
