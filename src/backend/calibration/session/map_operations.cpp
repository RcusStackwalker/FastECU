#include "src/backend/calibration/session/map_operations.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "src/backend/calibration/definition_interpretation_internal.h"
#include "src/backend/calibration/session/numeric_target_internal.h"

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
    return internal::ResolveBlobSelection(definition, map, selection)
        .and_then(
            [&](const std::variant<internal::BlobPatch, internal::BlobMiss>& resolved) -> Result<SelectableEditOutcome>
            {
                if (const auto *miss = std::get_if<internal::BlobMiss>(&resolved); miss != nullptr)
                {
                    return NotApplicable(*miss == internal::BlobMiss::kNotABlob
                                             ? NotApplicableReason::kUnavailableTarget
                                             : NotApplicableReason::kUnknownSelection);
                }
                const auto& [offset, data] = std::get<internal::BlobPatch>(resolved);
                if (const auto current = session.Read(offset, data.size());
                    current.has_value() && std::ranges::equal(data, *current))
                {
                    return SelectableEditOutcome{SelectableEditUnchanged{}};
                }
                return session.WriteBytes(offset, data)
                    .transform([] { return SelectableEditOutcome{SelectableEditChanged{}}; });
            });
}

} // namespace fastecu::calibration
