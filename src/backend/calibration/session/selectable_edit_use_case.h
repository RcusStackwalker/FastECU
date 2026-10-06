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

// The selection's bytes were written; the session is dirty. Writing bytes
// identical to the current ones also counts as changed.
struct SelectableEditChanged
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

using SelectableEditOutcome = std::variant<SelectableEditChanged, SelectableEditNotApplicable>;

// Resolves the session when called. When several selections share the
// requested name, the first one wins. The written width is the blob's element
// width: the first selection's hex length in bytes, so a longer value is
// truncated and a shorter one is zero-padded to it. A map without an address
// writes at offset 0. A write the image rejects is an error and changes
// nothing. Synchronous; the caller owns the execution context.
Result<SelectableEditOutcome> apply_selectable_edit(CalibrationWorkspace& workspace,
                                                    const SelectableEditRequest& request);

} // namespace fastecu::calibration
