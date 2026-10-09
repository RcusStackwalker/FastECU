#include "src/backend/calibration/session/selectable_edit_use_case.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/definition/definition_model.h"

namespace fastecu::calibration
{
namespace
{

SelectableEditOutcome NotApplicable(SelectableNotApplicableReason reason)
{
    return SelectableEditNotApplicable{.reason = reason};
}

} // namespace

Result<SelectableEditOutcome> ApplySelectableEdit(CalibrationWorkspace& workspace, const SelectableEditRequest& request)
{
    CalibrationSession *session = workspace.Find(request.session);
    if (session == nullptr)
    {
        return NotApplicable(SelectableNotApplicableReason::kClosedSession);
    }
    if (session->Definition() == nullptr)
    {
        return NotApplicable(SelectableNotApplicableReason::kNoDefinition);
    }
    const auto& definition = session->Definition()->definition;
    if (request.map_index >= definition.maps.size())
    {
        return NotApplicable(SelectableNotApplicableReason::kUnknownMap);
    }
    const auto& map = definition.maps[request.map_index];
    const auto *scaling = definition::FindScaling(definition, map.scaling_name);
    const auto storage = map.storage_type.has_value() ? map.storage_type
                         : scaling != nullptr         ? scaling->storage_type
                                                      : std::nullopt;
    if (scaling == nullptr || scaling->selections.empty() || storage != definition::StorageType::kBloblist)
    {
        return NotApplicable(SelectableNotApplicableReason::kNotBloblist);
    }
    const auto selected = std::ranges::find(scaling->selections, request.selection, &definition::Selection::name);
    if (selected == scaling->selections.end())
    {
        return NotApplicable(SelectableNotApplicableReason::kUnknownSelection);
    }
    const auto& data = selected->value;
    if (data.size() != ElementByteSize(storage, scaling))
    {
        return Fail(ErrorKind::kInvalidConfig, "selection value width differs from the blob width");
    }
    const auto offset = map.address.value_or(0);
    const auto image = session->Rom();
    if (offset <= image.size() && data.size() <= image.size() - offset &&
        std::ranges::equal(data, image.subspan(static_cast<std::size_t>(offset), data.size())))
    {
        return SelectableEditOutcome{SelectableEditUnchanged{}};
    }
    const auto written = session->WriteBytes(offset, data);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return SelectableEditOutcome{SelectableEditChanged{}};
}

} // namespace fastecu::calibration
