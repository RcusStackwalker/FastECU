#pragma once

#include <cstddef>
#include <string>
#include <variant>

#include "src/backend/calibration/map_edit.h"
#include "src/backend/calibration/numeric_map_edit.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Zero-based, inclusive element coordinates inside the target run. A body
// uses the map's geometry; an X axis is one row; a Y axis is one column.
struct NumericSelection
{
    NumericTarget target{NumericTarget::MapBody};
    SelectionRange elements;
};

struct IncrementEdit
{
    IncrementStep step{IncrementStep::FineUp};
};

// A checked expression; `x` is each cell's current scaled value.
struct AssignmentEdit
{
    std::string expression;
};

struct InterpolationEdit
{
    InterpolationMode mode{InterpolationMode::Horizontal};
};

using NumericEditOperation = std::variant<IncrementEdit, AssignmentEdit, InterpolationEdit>;

struct NumericEditRequest
{
    SessionId session{};
    std::size_t map_index{0};
    NumericSelection selection;
    NumericEditOperation operation;
};

// At least one stored byte changed; the session is dirty.
struct NumericEditChanged
{
};

// No stored byte differs; the session's dirty state is untouched.
struct NumericEditUnchanged
{
    NoChangeReason reason{NoChangeReason::Unchanged};
};

enum class NotApplicableReason
{
    ClosedSession,
    NoDefinition,
    // An unknown map, an absent or static axis, a blob body, or a map whose
    // layout cannot be decoded.
    UnavailableTarget,
};

struct NumericEditNotApplicable
{
    NotApplicableReason reason{NotApplicableReason::ClosedSession};
};

using NumericEditOutcome = std::variant<NumericEditChanged, NumericEditUnchanged, NumericEditNotApplicable>;

// Resolves the session when called, decodes the target from current bytes,
// calculates the edit and applies it atomically before returning. Invalid
// selections, expressions, values and encodings are InvalidConfig errors and
// change nothing. Synchronous; the caller owns the execution context.
Result<NumericEditOutcome> apply_numeric_edit(CalibrationWorkspace& workspace, const NumericEditRequest& request);

} // namespace fastecu::calibration
