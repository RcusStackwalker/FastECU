#pragma once

#include <cstddef>
#include <string_view>
#include <variant>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/numeric_operation_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// One map of one open calibration.
struct MapRef
{
    SessionId session{};
    std::size_t map_index{0};
};

// The selection's bytes were written; the session is dirty.
struct SelectableEditChanged
{
};

// The selection's bytes already matched the image; nothing was written and the
// session's dirty state is untouched.
struct SelectableEditUnchanged
{
};

// Not applicable also covers a map with no selections or one not stored as a
// blob (kUnavailableTarget), and a name no selection carries (kUnknownSelection).
using SelectableEditOutcome = std::variant<SelectableEditChanged, SelectableEditUnchanged, MapNotApplicable>;

// Every operation resolves the session when called, decodes the target from
// current bytes, and applies or reads it before returning. A closed session, a
// missing definition, an unknown map or an unavailable target is a
// MapNotApplicable outcome. Invalid selections, expressions, values and
// encodings are InvalidConfig errors and change nothing. Synchronous; the
// caller owns the execution context.

// Increment, Set Value, interpolation or Paste on a map body or axis. Edits are
// atomic: every write is validated before any byte changes.
Result<NumericEditOutcome> EditNumeric(CalibrationWorkspace& workspace, MapRef map, const NumericSelection& selection,
                                       NumericEditOperation operation);

// Reads the selected scaled values as text. Never changes the session.
Result<NumericCopyOutcome> CopyNumeric(CalibrationWorkspace& workspace, MapRef map, const NumericSelection& selection);

// Writes the named selection's bytes to a selectable map's address. The blob's
// width is the first selection's byte length; a selection of any other width,
// and a write the image rejects, is an error. A map without an address writes
// at offset 0.
Result<SelectableEditOutcome> EditSelectable(CalibrationWorkspace& workspace, MapRef map, std::string_view selection);

} // namespace fastecu::calibration
