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
    ClosedSession,
    NoDefinition,
    UnknownMap,
    // The map has no selections or is not stored as a blob, such as a
    // multi-selectable map.
    NotBloblist,
    // No selection carries the requested name.
    UnknownSelection,
};

struct SelectableEditNotApplicable
{
    SelectableNotApplicableReason reason{SelectableNotApplicableReason::ClosedSession};
};

using SelectableEditOutcome = std::variant<SelectableEditChanged, SelectableEditUnchanged, SelectableEditNotApplicable>;

// Resolves the session when called. When several selections share the
// requested name, the first one wins. The blob's width is the first
// selection's hex length in bytes. A map without an address writes at offset 0.
// The selection's value must be whole hexadecimal bytes of exactly that width;
// otherwise, and for a write the image rejects, it is an error and changes
// nothing. Synchronous; the caller owns the execution context.
Result<SelectableEditOutcome> apply_selectable_edit(CalibrationWorkspace& workspace,
                                                    const SelectableEditRequest& request);

} // namespace fastecu::calibration
