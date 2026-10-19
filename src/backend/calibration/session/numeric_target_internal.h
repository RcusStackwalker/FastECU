#pragma once

#include <cstddef>
#include <variant>

#include "src/backend/calibration/decoded_map.h"
#include "src/backend/calibration/definition_interpretation_internal.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/numeric_operation_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration::internal
{

// The session, definition and map a request names, resolved for one
// synchronous operation. Borrows from the workspace: do not store it.
struct LocatedMap
{
    CalibrationSession& session;
    const definition::RomDefinition& definition;
    const definition::CalibrationMap& map;
};

// Resolves a session, its definition and a map index, in that order. Each miss
// is an outcome, not an error.
std::variant<LocatedMap, MapNotApplicable> LocateMap(CalibrationWorkspace& workspace, SessionId session,
                                                     std::size_t map_index);

// Owns metadata and decoded cells for one synchronous operation. Moves retain
// ownership; borrowed edit views are created only inside Apply. The session
// must remain open, and this target must not be persisted across operations.
class PreparedNumericTarget
{
  public:
    Result<NumericCopyOutcome> Copy(const SelectionRange& range) const;
    Result<NumericEditOutcome> Apply(const SelectionRange& range, const NumericEditOperation& operation);

  private:
    friend std::variant<PreparedNumericTarget, MapNotApplicable> PrepareNumericTarget(CalibrationWorkspace&, SessionId,
                                                                                      std::size_t, NumericTarget);

    PreparedNumericTarget(CalibrationSession& session, MapElement element, NumericRun run);

    MapElement element_;
    NumericRun run_;
    CalibrationSession *session_;
};

using NumericTargetPreparation = std::variant<PreparedNumericTarget, MapNotApplicable>;
NumericTargetPreparation PrepareNumericTarget(CalibrationWorkspace& workspace, SessionId session, std::size_t map_index,
                                              NumericTarget target);

} // namespace fastecu::calibration::internal
