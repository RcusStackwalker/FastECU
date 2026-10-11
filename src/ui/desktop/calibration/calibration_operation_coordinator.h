#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/rom_save.h"
#include "src/backend/config/catalog.h"
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
    config::ProtocolSpec protocol;
    std::string kernel_path;
    std::string display_filename;
};

enum class SaveMode
{
    // Overwrites the session's current source path.
    kSave,
    // Asks the operator for a destination after checksum interaction.
    kSaveAs
};

enum class SaveOutcome
{
    kSaved,
    // The operator dismissed the Save As picker or chose no filename;
    // nothing was written.
    kCancelled,
    // No session was selected; only the missing-calibration notice was shown.
    kNoSelection,
    // The repository refused the write; the session is unchanged.
    kFailed
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
    std::optional<PreparedWrite> prepareWrite(calibration::CalibrationSession *session,
                                              std::string_view kernelDirectory);

    // Persists a copy of the session bytes, checksum-corrected when correction
    // produced bytes, through RomSaveUseCase, which alone updates the saved
    // source and dirty state and reports a repository failure. Saving does not
    // refresh write metadata. After a Saved Save As, the session's
    // source().display_name names the new file; the owner refreshes any label
    // showing it.
    SaveOutcome save(calibration::CalibrationSession *session, SaveMode mode);

  private:
    void refreshWriteMetadata(calibration::CalibrationSession& session, std::string_view kernelDirectory);
    void correctOperationImage(const calibration::CalibrationSession& session, bytes::Bytes& image);
    std::optional<std::string> chooseSaveAsPath(const calibration::CalibrationSession& session);

    config::ConfigSession& config_;
    calibration::RomSaveUseCase& saver_;
    ICalibrationInteraction& interaction_;
    CalibrationPresentationCallbacks callbacks_;
};

} // namespace fastecu::ui
