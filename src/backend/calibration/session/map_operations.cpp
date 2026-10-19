#include "src/backend/calibration/session/map_operations.h"

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

Result<NumericEditOutcome> EditNumeric(CalibrationWorkspace& workspace, MapRef map, const NumericSelection& selection,
                                       NumericEditOperation operation)
{
    auto preparation = internal::PrepareNumericTarget(workspace, map.session, map.map_index, selection.target);
    if (auto *target = std::get_if<internal::PreparedNumericTarget>(&preparation); target != nullptr)
    {
        return target->Apply(selection.elements, operation);
    }
    return NumericEditOutcome{std::get<MapNotApplicable>(preparation)};
}

Result<NumericCopyOutcome> CopyNumeric(CalibrationWorkspace& workspace, MapRef map, const NumericSelection& selection)
{
    auto preparation = internal::PrepareNumericTarget(workspace, map.session, map.map_index, selection.target);
    if (auto *target = std::get_if<internal::PreparedNumericTarget>(&preparation); target != nullptr)
    {
        return target->Copy(selection.elements);
    }
    return NumericCopyOutcome{std::get<MapNotApplicable>(preparation)};
}

Result<SelectableEditOutcome> EditSelectable(CalibrationWorkspace& workspace, MapRef map_ref,
                                             std::string_view selection)
{
    const auto located = internal::LocateMap(workspace, map_ref.session, map_ref.map_index);
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
    const auto selected = std::ranges::find(element.selections, selection, &definition::Selection::name);
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
