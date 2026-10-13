#include "src/backend/calibration/session/numeric_copy_use_case.h"

#include "src/backend/calibration/session/numeric_target_internal.h"

namespace fastecu::calibration
{

Result<NumericCopyOutcome> CopyNumericValues(CalibrationWorkspace& workspace, const NumericCopyRequest& request)
{
    auto preparation =
        internal::PrepareNumericTarget(workspace, request.session, request.map_index, request.selection.target);
    if (auto *target = std::get_if<internal::PreparedNumericTarget>(&preparation); target != nullptr)
    {
        return target->Copy(request.selection.elements);
    }
    return NumericCopyOutcome{std::get<NumericEditNotApplicable>(preparation)};
}

} // namespace fastecu::calibration
