#include "src/backend/calibration/session/selectable_edit_use_case.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/definition_interpretation_internal.h"
#include "src/backend/calibration/session/numeric_target_internal.h"
#include "src/backend/definition/definition_model.h"

namespace fastecu::calibration
{
namespace
{

SelectableEditOutcome NotApplicable(NotApplicableReason reason)
{
    return MapNotApplicable{.reason = reason};
}

} // namespace

Result<SelectableEditOutcome> ApplySelectableEdit(CalibrationWorkspace& workspace, const SelectableEditRequest& request)
{
    const auto located = internal::LocateMap(workspace, request.session, request.map_index);
    if (const auto *miss = std::get_if<MapNotApplicable>(&located); miss != nullptr)
    {
        return SelectableEditOutcome{*miss};
    }
    const auto& [session, definition, map] = std::get<internal::LocatedMap>(located);
    const auto element = internal::ResolveMapElement(definition, map, NumericTarget::kMapBody);
    if (!element.IsBlobSelection())
    {
        return NotApplicable(NotApplicableReason::kUnavailableTarget);
    }
    const auto selected = std::ranges::find(element.selections, request.selection, &definition::Selection::name);
    if (selected == element.selections.end())
    {
        return NotApplicable(NotApplicableReason::kUnknownSelection);
    }
    const auto& data = selected->value;
    if (data.size() != element.byte_width)
    {
        return Fail(ErrorKind::kInvalidConfig, "selection value width differs from the blob width");
    }
    const std::uint64_t offset = element.address.has_value() ? element.address->Value() : 0;
    if (const auto current = session.Read(offset, data.size());
        current.has_value() && std::ranges::equal(data, *current))
    {
        return SelectableEditOutcome{SelectableEditUnchanged{}};
    }
    const auto written = session.WriteBytes(offset, data);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return SelectableEditOutcome{SelectableEditChanged{}};
}

} // namespace fastecu::calibration
