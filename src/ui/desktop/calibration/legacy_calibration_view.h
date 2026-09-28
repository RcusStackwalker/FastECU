#pragma once

#include <memory>
#include <optional>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

namespace fastecu::ui
{

// Transitional (step 6m-2, removed in 6m-3). The legacy struct every desktop
// calibration reader and writer still takes, built once from an opened
// session. It is what FileActions::open_subaru_rom_file left in MainWindow's
// slot, and until 6m-3 it is the only byte store the UI reads or writes.
struct LegacyCalibrationView
{
    std::unique_ptr<FileActions::EcuCalDefStructure> view;
    // First map-decode failure; that map keeps its placeholder text, as legacy.
    std::optional<Error> decode_error;
};

Result<LegacyCalibrationView> project_legacy_calibration(const calibration::CalibrationSession& session);

// One-way copy of the session's ROM metadata into a legacy view (step 6m-3).
// The session is the only writer; readers that still take the legacy struct
// see the refreshed values.
void refresh_legacy_metadata(const calibration::CalibrationSession& session, FileActions::EcuCalDefStructure& legacy);

} // namespace fastecu::ui
