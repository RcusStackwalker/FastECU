#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/rom_save.h"
#include "src/backend/config/config_session.h"
#include "src/backend/ports/event_sink.h"
#include "src/ui/desktop/calibration/calibration_interaction.h"

namespace fastecu::ui
{

// What a flash write dispatches: the operation image, checksum-corrected when
// correction produced bytes, and the metadata read after the write refresh.
struct PreparedWrite
{
    bytes::Bytes image;
    std::string protocol;
    std::string mcu;
    std::string kernel_path;
    std::string display_filename;
};

enum class SaveMode
{
    // Overwrites the session's current source path.
    Save,
    // Asks the operator for a destination after checksum interaction.
    SaveAs
};

enum class SaveOutcome
{
    Saved,
    // The operator dismissed the Save As picker; nothing was written.
    Cancelled,
    NoSelection,
    // The repository refused the write; the session is unchanged.
    Failed
};

// Presentation effects the coordinator asks of its owner. Both are required
// and are called synchronously.
struct CalibrationPresentationCallbacks
{
    std::function<void(LogLevel, std::string_view)> log;
    // The selected vehicle's protocol description, after write preparation
    // reselected the vehicle.
    std::function<void(std::string_view)> protocol_description_changed;
};

// Sequences calibration operations for one session at a time: the pre-write
// checksum warning, the write-metadata refresh, Save/Save As, and checksum
// correction of an operation image that never replaces the editable session
// bytes. Borrows its services and retains no session between calls. The
// configuration session must be initialized.
class CalibrationOperationCoordinator
{
  public:
    CalibrationOperationCoordinator(config::ConfigSession& config, calibration::RomSaveUseCase& saver,
                                    ICalibrationInteraction& interaction, CalibrationPresentationCallbacks callbacks);

    // nullopt when no session is selected or the operator cancels the
    // missing-checksum-module warning. Declining checksum correction still
    // prepares the uncorrected image.
    std::optional<PreparedWrite> prepare_write(calibration::CalibrationSession *session,
                                               std::string_view kernel_directory);

    // Persists a copy of the session bytes, checksum-corrected when correction
    // produced bytes, through RomSaveUseCase, which alone updates the saved
    // source and dirty state and reports a repository failure. Saving does not
    // refresh write metadata.
    SaveOutcome save(calibration::CalibrationSession *session, SaveMode mode);

  private:
    void refresh_write_metadata(calibration::CalibrationSession& session, std::string_view kernel_directory);
    void correct_operation_image(const calibration::CalibrationSession& session, bytes::Bytes& image);
    std::optional<std::string> choose_save_as_path(const calibration::CalibrationSession& session);

    config::ConfigSession& config_;
    calibration::RomSaveUseCase& saver_;
    ICalibrationInteraction& interaction_;
    CalibrationPresentationCallbacks callbacks_;
};

} // namespace fastecu::ui
