#include "src/backend/calibration/session/numeric_edit_use_case.h"

#include "src/backend/calibration/session/numeric_target_internal.h"

namespace fastecu::calibration
{

Result<NumericEditOutcome> ApplyNumericEdit(CalibrationWorkspace& workspace, const NumericEditRequest& request)
{
    auto preparation =
        internal::PrepareNumericTarget(workspace, request.session, request.map_index, request.selection.target);
    if (auto *target = std::get_if<internal::PreparedNumericTarget>(&preparation); target != nullptr)
    {
        return target->Apply(request.selection.elements, request.operation);
    }
    return NumericEditOutcome{std::get<MapNotApplicable>(preparation)};
}

} // namespace fastecu::calibration
