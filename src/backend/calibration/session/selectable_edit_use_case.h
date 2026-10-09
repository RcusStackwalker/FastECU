#pragma once

#include <cstddef>
#include <string>
#include <variant>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Writes the named selection's bytes to a selectable map's address.
struct SelectableEditRequest
{
    SessionId session{};
    std::size_t map_index{0};
    std::string selection;
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

enum class SelectableNotApplicableReason
{
    kClosedSession,
    kNoDefinition,
    kUnknownMap,
    // The map has no selections or is not stored as a blob, such as a
    // multi-selectable map.
    kNotBloblist,
    // No selection carries the requested name.
    kUnknownSelection,
};

struct SelectableEditNotApplicable
{
    SelectableNotApplicableReason reason{SelectableNotApplicableReason::kClosedSession};
};

using SelectableEditOutcome = std::variant<SelectableEditChanged, SelectableEditUnchanged, SelectableEditNotApplicable>;

// Resolves the session when called. The blob's width is the first selection's
// byte length; a selection of any other width, and a write the image rejects,
// is an error and changes nothing. A map without an address writes at offset 0.
// Synchronous; the caller owns the execution context.
Result<SelectableEditOutcome> apply_selectable_edit(CalibrationWorkspace& workspace,
                                                    const SelectableEditRequest& request);

} // namespace fastecu::calibration
