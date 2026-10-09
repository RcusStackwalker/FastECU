#pragma once

#include <cstddef>
#include <string>
#include <variant>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/numeric_edit_use_case.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

struct NumericCopyRequest
{
    SessionId session{};
    std::size_t map_index{0};
    NumericSelection selection;
};

// Tab-separated cells, LF-separated rows, row-major from the selection's
// top-left. Each cell is the full-precision scaled value in locale-independent
// plain decimal, so pasting it back decodes to the same double.
struct NumericCopyText
{
    std::string text;
};

// The selection holds a cell whose current ROM content yields no scaled value;
// a clipboard entry containing it could not be pasted. Coordinates are
// zero-based inside the target run, like NumericSelection.
struct NumericCopyInvalidCell
{
    int row{0};
    int col{0};
    std::string detail;
};

using NumericCopyOutcome = std::variant<NumericCopyText, NumericCopyInvalidCell, NumericEditNotApplicable>;

// Resolves the session when called and decodes the target from current bytes.
// A selection that is empty or leaves the target run is an InvalidConfig error.
// Reads only; the session is never changed. Synchronous.
Result<NumericCopyOutcome> CopyNumericValues(CalibrationWorkspace& workspace, const NumericCopyRequest& request);

} // namespace fastecu::calibration
