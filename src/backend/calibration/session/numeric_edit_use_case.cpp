#include "src/backend/calibration/session/numeric_edit_use_case.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/algorithms/expression/expression.h"
#include "src/backend/calibration/session/map_element_fields.h"

namespace fastecu::calibration
{
namespace
{

NumericEditOutcome NotApplicable(NotApplicableReason reason)
{
    return NumericEditNotApplicable{.reason = reason};
}

const NumericRun *TargetCells(const DecodedMap& values, NumericTarget target)
{
    switch (target)
    {
    case NumericTarget::kMapBody:
        return std::get_if<NumericRun>(&values.body);
    case NumericTarget::kXAxis:
        return std::get_if<NumericRun>(&values.x_axis);
    case NumericTarget::kYAxis:
        return std::get_if<NumericRun>(&values.y_axis);
    }
    return nullptr;
}

// A Y axis is one column; a body or X axis is as wide as the map.
std::uint32_t RunWidth(const MapElementSpec& spec, NumericTarget target)
{
    return target == NumericTarget::kYAxis ? 1U : spec.x_size;
}

// An X axis is one row; a body or Y axis is as tall as the map.
std::uint32_t RunHeight(const MapElementSpec& spec, NumericTarget target)
{
    return target == NumericTarget::kXAxis ? 1U : spec.y_size;
}

// Validates every supplied cell, including those clipping will discard.
Result<std::vector<std::vector<double>>> ParsePaste(const PasteEdit& paste)
{
    std::vector<std::vector<double>> rows;
    rows.reserve(paste.rows.size());
    for (const auto& row : paste.rows)
    {
        std::vector<double> values;
        values.reserve(row.size());
        for (const auto& text : row)
        {
            const auto number = expression::ParseFiniteNumber(text);
            if (!number.has_value())
            {
                return Fail(ErrorKind::kInvalidConfig, number.error().detail);
            }
            values.push_back(*number);
        }
        rows.push_back(std::move(values));
    }
    return rows;
}

Result<NumericEditResult> Calculate(bytes::ByteView rom, const MapElementSpec& spec, const NumericSelection& selection,
                                    std::span<const NumericCell> cells, const NumericEditOperation& operation)
{
    const auto width = RunWidth(spec, selection.target);
    return std::visit(
        [&](const auto& edit) -> Result<NumericEditResult>
        {
            using Edit = std::decay_t<decltype(edit)>;
            if constexpr (std::is_same_v<Edit, IncrementEdit>)
            {
                return CalculateIncrement(rom, spec, width, cells, selection.elements, edit.step);
            }
            else if constexpr (std::is_same_v<Edit, AssignmentEdit>)
            {
                return CalculateAssignment(rom, spec, width, cells, selection.elements, edit.expression);
            }
            else if constexpr (std::is_same_v<Edit, InterpolationEdit>)
            {
                return CalculateInterpolation(rom, spec, width, cells, selection.elements, edit.mode);
            }
            else
            {
                static_assert(std::is_same_v<Edit, PasteEdit>);
                const auto values = ParsePaste(edit);
                if (!values.has_value())
                {
                    return std::unexpected(values.error());
                }
                return CalculatePaste(rom, spec, width, RunHeight(spec, selection.target), selection.elements, *values);
            }
        },
        operation);
}

// Validates every write against the target run before any byte changes, so
// one bad cell rejects the whole edit. Byte-identical writes are skipped,
// preserving clean state.
Status WritePatch(CalibrationSession& session, const MapElementSpec& spec, std::size_t cell_count,
                  const NumericEditPatch& patch)
{
    const auto width = definition::StorageByteSize(spec.storage_type);
    const auto size = session.Rom().size();
    for (const auto& cell : patch)
    {
        if (cell.index >= cell_count || cell.bytes.size() != width ||
            cell.byte_address != ElementByteAddress(spec, cell.index))
        {
            return Fail(ErrorKind::kInvalidConfig, "map edit index, address, or byte width does not match its target");
        }
        if (cell.byte_address > size || cell.bytes.size() > size - cell.byte_address)
        {
            return Fail(ErrorKind::kInvalidConfig, "map edit byte range is outside the ROM image");
        }
    }
    for (const auto& cell : patch)
    {
        const auto current = session.Rom().subspan(static_cast<std::size_t>(cell.byte_address), cell.bytes.size());
        if (std::ranges::equal(current, cell.bytes))
        {
            continue;
        }
        const auto written = session.WriteBytes(cell.byte_address, cell.bytes);
        if (!written.has_value())
        {
            return written;
        }
    }
    return {};
}

} // namespace

Result<NumericEditOutcome> ApplyNumericEdit(CalibrationWorkspace& workspace, const NumericEditRequest& request)
{
    CalibrationSession *session = workspace.Find(request.session);
    if (session == nullptr)
    {
        return NotApplicable(NotApplicableReason::kClosedSession);
    }
    if (session->Definition() == nullptr)
    {
        return NotApplicable(NotApplicableReason::kNoDefinition);
    }
    if (request.map_index >= session->Definition()->definition.maps.size())
    {
        return NotApplicable(NotApplicableReason::kUnavailableTarget);
    }
    const auto decoded = session->DecodeMap(request.map_index);
    if (!decoded.has_value())
    {
        return NotApplicable(NotApplicableReason::kUnavailableTarget);
    }
    const NumericRun *run = TargetCells(*decoded, request.selection.target);
    if (run == nullptr)
    {
        return NotApplicable(NotApplicableReason::kUnavailableTarget);
    }
    const auto fields = CollectMapElementFields(*session, request.map_index, request.selection.target);
    const auto spec = fields.Spec();
    const auto calculated = Calculate(session->Rom(), spec, request.selection, run->cells, request.operation);
    if (!calculated.has_value())
    {
        return std::unexpected(calculated.error());
    }
    if (calculated->writes.empty())
    {
        return NumericEditOutcome{NumericEditUnchanged{.reason = calculated->no_change}};
    }
    const auto written = WritePatch(*session, spec, run->cells.size(), calculated->writes);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return NumericEditOutcome{NumericEditChanged{}};
}

} // namespace fastecu::calibration
