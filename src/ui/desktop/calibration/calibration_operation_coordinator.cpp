#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"

#include <expected>
#include <format>
#include <optional>
#include <utility>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_error.h"
#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/memory/memory_map.h"
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

const config::VehicleSpec& selectedVehicle(const config::ConfigSession& config)
{
    // The desktop startup selection gate ensures a valid vehicle before this coordinator is used.
    return *config.SelectedVehicle();
}

const config::ProtocolSpec& selectedProtocol(const config::ConfigSession& config)
{
    return *selectedVehicle(config).protocol;
}

// Legacy Save As normalization: drop one trailing dot, then append ".bin"
// unless the name already ends in lowercase ".bin". Nothing else changes.
std::string withCalibrationSuffix(std::string path)
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

// The selected protocol's checksum route addresses the ROM through that
// protocol's memory map for this file size. A size it declares no map for is
// placed by the identity map, so the romsize check still reports it as a bad
// ROM size.
std::expected<memory::MemoryImage, memory::MemoryError> checksumImage(const config::ProtocolSpec& protocol,
                                                                      bytes::ByteView file)
{
    auto placed = config::PlaceRomFile(&protocol, bytes::Bytes(file.begin(), file.end()));
    if (placed.has_value())
    {
        return placed;
    }
    return config::PlaceRomFile(nullptr, bytes::Bytes(file.begin(), file.end()));
}
} // namespace

CalibrationOperationCoordinator::CalibrationOperationCoordinator(config::ConfigSession& config,
                                                                 calibration::RomSaveUseCase& saver,
                                                                 ICalibrationInteraction& interaction,
                                                                 CalibrationPresentationCallbacks callbacks)
    : config_(config), saver_(saver), interaction_(interaction), callbacks_(std::move(callbacks))
{
}

std::optional<PreparedWrite> CalibrationOperationCoordinator::prepareWrite(calibration::CalibrationSession *session,
                                                                           std::string_view kernelDirectory)
{
    if (session == nullptr)
    {
        interaction_.showNotice(CalibrationNotice::kNoCalibrationToWrite);
        return std::nullopt;
    }

    bytes::Bytes image(session->File().begin(), session->File().end());
    if (selectedProtocol(config_).checksum == config::ChecksumSupport::kMissing &&
        !interaction_.confirmWriteWithoutChecksum())
    {
        callbacks_.log(LogLevel::kDebug, "Write canceled!");
        return std::nullopt;
    }
    refreshWriteMetadata(*session, kernelDirectory);

    // Read again: the refresh may have reselected the vehicle.
    if (selectedProtocol(config_).checksum != config::ChecksumSupport::kMissing)
    {
        correctOperationImage(*session, image);
    }
    return PreparedWrite{
        .image = std::move(image),
        .protocol = selectedProtocol(config_),
        .kernel_path = session->Protocol().kernel_path,
        .display_filename = session->Source().display_name,
    };
}

SaveOutcome CalibrationOperationCoordinator::save(calibration::CalibrationSession *session, SaveMode mode)
{
    if (session == nullptr)
    {
        interaction_.showNotice(CalibrationNotice::kNoCalibrationToSave);
        return SaveOutcome::kNoSelection;
    }
    if (mode == SaveMode::kSaveAs)
    {
        callbacks_.log(LogLevel::kDebug, "Save as: Check selected ROM number");
    }
    bytes::Bytes image(session->File().begin(), session->File().end());
    correctOperationImage(*session, image);

    const std::optional<std::string> target =
        mode == SaveMode::kSave ? std::optional{session->Source().path} : chooseSaveAsPath(*session);
    if (!target.has_value())
    {
        return SaveOutcome::kCancelled;
    }
    // RomSaveUseCase logs and notices a repository failure itself; the
    // operator gets no second notice from here.
    if (!saver_.Save(*session, *target, image).has_value())
    {
        callbacks_.log(LogLevel::kError, std::format("Calibration file not saved: {}", *target));
        return SaveOutcome::kFailed;
    }
    callbacks_.log(LogLevel::kDebug, std::format("ecuCalDef->FileName: {}", session->Source().display_name));
    callbacks_.log(LogLevel::kDebug, std::format("ecuCalDef->FullFileName: {}", session->Source().path));
    return SaveOutcome::kSaved;
}

// Legacy filled an empty flash method only when a definition left it empty --
// a definitionless ROM shows the " " placeholder -- and reselected the vehicle
// by that name before taking kernel and MCU from the selection. These values
// reach the ECU.
void CalibrationOperationCoordinator::refreshWriteMetadata(calibration::CalibrationSession& session,
                                                           std::string_view kernelDirectory)
{
    calibration::RomProtocolInfo protocol = session.Protocol();
    if (session.Definition() != nullptr && protocol.flash_method.empty())
    {
        protocol.flash_method = std::string(selectedProtocol(config_).name);
        session.SetProtocol(protocol);
        // Mirrored in MainWindow::update_protocol_info; keep the two in sync.
        callbacks_.log(LogLevel::kDebug,
                       std::format("Update protocol info by selected ROM with FlashMethod: {}", protocol.flash_method));
        // The last matching row wins, as the legacy scan did; no match changes
        // nothing.
        if (config_.SelectByProtocolName(protocol.flash_method))
        {
            callbacks_.log(LogLevel::kDebug, "Protocol info for selected ROM updated");
        }
        else
        {
            callbacks_.log(LogLevel::kDebug, "Could not find protocol for selected ROM!");
        }
        callbacks_.protocol_description_changed(selectedProtocol(config_).description);
    }
    protocol.kernel_path = flash::KernelPath(kernelDirectory, selectedProtocol(config_).kernel);
    protocol.kernel_start_address = config::KernelLoadAddressText(selectedProtocol(config_));
    protocol.mcu_type = std::string(selectedProtocol(config_).mcu);
    session.SetProtocol(protocol);
}

// Corrected bytes replace only the operation image, never the editable
// session. An unknown MCU, a declined correction, or an outcome without
// corrected bytes leaves the image as it was.
void CalibrationOperationCoordinator::correctOperationImage(const calibration::CalibrationSession& session,
                                                            bytes::Bytes& image)
{
    const config::VehicleSpec& vehicle = selectedVehicle(config_);
    const checksum::ChecksumSelection selection{
        .make = std::string(vehicle.make),
        .checksum_flag = std::string(config::ChecksumFlag(vehicle.protocol->checksum)),
        .flash_method = std::string(vehicle.protocol->name),
        .mcu_type = session.Protocol().mcu_type,
        .rom_id = session.Protocol().rom_id,
    };
    callbacks_.log(LogLevel::kDebug, std::format("Protocol: {}", selection.flash_method));
    callbacks_.log(LogLevel::kDebug, std::format("Make: {}", selection.make));
    callbacks_.log(LogLevel::kDebug, std::format("Checksum: {}", selection.checksum_flag));
    if (const auto *device = flash::FindFlashDevice(selection.mcu_type); device != nullptr)
    {
        callbacks_.log(LogLevel::kDebug,
                       std::format("ecuCalDef->McuType: {} {}", session.Protocol().mcu_type, vehicle.protocol->mcu));
        callbacks_.log(LogLevel::kDebug, std::format("Size: 0x{:x} -> 0x{:x}", image.size(), device->romsize));
    }
    const auto placed = checksumImage(*vehicle.protocol, image);
    if (!placed.has_value())
    {
        callbacks_.log(LogLevel::kError, std::format("Checksum image error: {}", placed.error().detail));
        return;
    }
    const ChecksumCorrectionResult result =
        interaction_.correctChecksums(*placed, session.Definition() != nullptr, selection);
    if (result.unknown_mcu_type)
    {
        callbacks_.log(LogLevel::kError, std::format("Unknown MCU type: {}", session.Protocol().mcu_type));
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
CalibrationOperationCoordinator::chooseSaveAsPath(const calibration::CalibrationSession& session)
{
    callbacks_.log(LogLevel::kDebug, "Save as: Check if OEM ECU file");
    const std::optional<std::string> chosen = interaction_.chooseSavePath(
        config_.EffectivePaths().calibration_files_directory + session.Source().display_name);
    if (!chosen.has_value() || chosen->empty())
    {
        interaction_.showNotice(CalibrationNotice::kNoSaveFilename);
        return std::nullopt;
    }
    return withCalibrationSuffix(*chosen);
}

} // namespace fastecu::ui
