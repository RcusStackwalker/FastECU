#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "src/backend/calibration/map_edit.h"
#include "src/backend/calibration/numeric_map_edit.h"
#include "src/backend/calibration/session/calibration_session.h"

namespace fastecu::calibration
{

// Zero-based, inclusive element coordinates inside the target run. A body
// uses the map's geometry; an X axis is one row; a Y axis is one column.
struct NumericSelection
{
    NumericTarget target{NumericTarget::kMapBody};
    SelectionRange elements;
};

struct IncrementEdit
{
    IncrementStep step{IncrementStep::kFineUp};
};

// A checked expression; `x` is each cell's current scaled value.
struct AssignmentEdit
{
    std::string expression;
};

struct InterpolationEdit
{
    InterpolationMode mode{InterpolationMode::kHorizontal};
};

// Source rows of unparsed text cells. Rows may be ragged and cells empty.
// Every cell must be a dot-decimal number, including cells the destination
// clips away; pasting starts at the selection's top-left element and clips
// to the target run's edges.
struct PasteEdit
{
    std::vector<std::vector<std::string>> rows;
};

using NumericEditOperation = std::variant<IncrementEdit, AssignmentEdit, InterpolationEdit, PasteEdit>;

// At least one stored byte changed; the session is dirty.
struct NumericEditChanged
{
};

// No stored byte differs; the session's dirty state is untouched.
struct NumericEditUnchanged
{
    NoChangeReason reason{NoChangeReason::kUnchanged};
};

// Why an operation does not apply to its map. Shared by every map operation:
// a UI element can outlive its session or its map, so these are outcomes, not
// errors.
enum class NotApplicableReason
{
    kClosedSession,
    kNoDefinition,
    kUnknownMap,
    // An absent or static axis, a blob body for a numeric operation, a map
    // that is not a blob selection for a selectable one, or a map whose layout
    // cannot be decoded.
    kUnavailableTarget,
    // A selectable edit naming a selection the map does not carry.
    kUnknownSelection,
};

struct MapNotApplicable
{
    NotApplicableReason reason{NotApplicableReason::kClosedSession};
};

using NumericEditOutcome = std::variant<NumericEditChanged, NumericEditUnchanged, MapNotApplicable>;

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

using NumericCopyOutcome = std::variant<NumericCopyText, NumericCopyInvalidCell, MapNotApplicable>;

} // namespace fastecu::calibration
