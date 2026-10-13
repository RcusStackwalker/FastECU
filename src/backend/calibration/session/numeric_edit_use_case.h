#pragma once

#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/numeric_operation_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Resolves the session when called, decodes the target from current bytes,
// calculates the edit and applies it atomically before returning. Invalid
// selections, expressions, values and encodings are InvalidConfig errors and
// change nothing. Synchronous; the caller owns the execution context.
Result<NumericEditOutcome> ApplyNumericEdit(CalibrationWorkspace& workspace, const NumericEditRequest& request);

} // namespace fastecu::calibration
