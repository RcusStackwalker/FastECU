#pragma once

#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/numeric_edit_use_case.h"
#include "src/backend/calibration/session/numeric_operation_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Resolves the session when called and decodes the target from current bytes.
// A selection that is empty or leaves the target run is an InvalidConfig error.
// Reads only; the session is never changed. Synchronous.
Result<NumericCopyOutcome> CopyNumericValues(CalibrationWorkspace& workspace, const NumericCopyRequest& request);

} // namespace fastecu::calibration
