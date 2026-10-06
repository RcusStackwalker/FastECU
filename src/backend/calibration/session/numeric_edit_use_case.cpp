#include "src/backend/calibration/session/numeric_edit_use_case.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/algorithms/expression/checked_expression.h"
#include "src/backend/calibration/session/map_element_fields.h"

namespace fastecu::calibration
{
namespace
{

NumericEditOutcome not_applicable(NotApplicableReason reason)
{
    return NumericEditNotApplicable{.reason = reason};
}

const NumericRun *target_cells(const DecodedMap& values, NumericTarget target)
{
    switch (target)
    {
    case NumericTarget::MapBody:
        return std::get_if<NumericRun>(&values.body);
    case NumericTarget::XAxis:
        return std::get_if<NumericRun>(&values.x_axis);
    case NumericTarget::YAxis:
        return std::get_if<NumericRun>(&values.y_axis);
    }
    return nullptr;
}

// A Y axis is one column; a body or X axis is as wide as the map.
std::uint32_t run_width(const MapElementSpec& spec, NumericTarget target)
{
    return target == NumericTarget::YAxis ? 1U : spec.x_size;
}

// An X axis is one row; a body or Y axis is as tall as the map.
std::uint32_t run_height(const MapElementSpec& spec, NumericTarget target)
{
    return target == NumericTarget::XAxis ? 1U : spec.y_size;
}

// Validates every supplied cell, including those clipping will discard.
Result<std::vector<std::vector<double>>> parse_paste(const PasteEdit& paste)
{
    std::vector<std::vector<double>> rows;
    rows.reserve(paste.rows.size());
    for (const auto& row : paste.rows)
    {
        std::vector<double> values;
        values.reserve(row.size());
        for (const auto& text : row)
        {
            const auto number = expression::parse_finite_number(text);
            if (!number.has_value())
            {
                return fail(ErrorKind::InvalidConfig, number.error().detail);
            }
            values.push_back(*number);
        }
        rows.push_back(std::move(values));
    }
    return rows;
}

Result<NumericEditResult> calculate(bytes::ByteView rom, const MapElementSpec& spec, const NumericSelection& selection,
                                    std::span<const NumericCell> cells, const NumericEditOperation& operation)
{
    const auto width = run_width(spec, selection.target);
    return std::visit(
        [&](const auto& edit) -> Result<NumericEditResult>
        {
            using Edit = std::decay_t<decltype(edit)>;
            if constexpr (std::is_same_v<Edit, IncrementEdit>)
            {
                return calculate_increment(rom, spec, width, cells, selection.elements, edit.step);
            }
            else if constexpr (std::is_same_v<Edit, AssignmentEdit>)
            {
                return calculate_assignment(rom, spec, width, cells, selection.elements, edit.expression);
            }
            else if constexpr (std::is_same_v<Edit, InterpolationEdit>)
            {
                return calculate_interpolation(rom, spec, width, cells, selection.elements, edit.mode);
            }
            else
            {
                static_assert(std::is_same_v<Edit, PasteEdit>);
                const auto values = parse_paste(edit);
                if (!values.has_value())
                {
                    return std::unexpected(values.error());
                }
                return calculate_paste(rom, spec, width, run_height(spec, selection.target), selection.elements,
                                       *values);
            }
        },
        operation);
}

// Validates every write against the target run before any byte changes, so
// one bad cell rejects the whole edit. Byte-identical writes are skipped,
// preserving clean state.
Status write_patch(CalibrationSession& session, const MapElementSpec& spec, std::size_t cell_count,
                   const NumericEditPatch& patch)
{
    const auto width = definition::storage_byte_size(spec.storage_type);
    const auto size = session.rom().size();
    for (const auto& cell : patch)
    {
        if (cell.index >= cell_count || cell.bytes.size() != width ||
            cell.byte_address != element_byte_address(spec, cell.index, true))
        {
            return fail(ErrorKind::InvalidConfig, "map edit index, address, or byte width does not match its target");
        }
        if (cell.byte_address > size || cell.bytes.size() > size - cell.byte_address)
        {
            return fail(ErrorKind::InvalidConfig, "map edit byte range is outside the ROM image");
        }
    }
    for (const auto& cell : patch)
    {
        const auto current = session.rom().subspan(static_cast<std::size_t>(cell.byte_address), cell.bytes.size());
        if (std::ranges::equal(current, cell.bytes))
        {
            continue;
        }
        const auto written = session.write_bytes(cell.byte_address, cell.bytes);
        if (!written.has_value())
        {
            return written;
        }
    }
    return {};
}

} // namespace

Result<NumericEditOutcome> apply_numeric_edit(CalibrationWorkspace& workspace, const NumericEditRequest& request)
{
    CalibrationSession *session = workspace.find(request.session);
    if (session == nullptr)
    {
        return not_applicable(NotApplicableReason::ClosedSession);
    }
    if (session->definition() == nullptr)
    {
        return not_applicable(NotApplicableReason::NoDefinition);
    }
    if (request.map_index >= session->definition()->definition.maps.size())
    {
        return not_applicable(NotApplicableReason::UnavailableTarget);
    }
    const auto decoded = session->decode_map(request.map_index);
    if (!decoded.has_value())
    {
        return not_applicable(NotApplicableReason::UnavailableTarget);
    }
    const NumericRun *run = target_cells(*decoded, request.selection.target);
    if (run == nullptr)
    {
        return not_applicable(NotApplicableReason::UnavailableTarget);
    }
    const auto fields = collect_map_element_fields(*session, request.map_index, request.selection.target);
    const auto spec = fields.spec();
    const auto calculated = calculate(session->rom(), spec, request.selection, run->cells, request.operation);
    if (!calculated.has_value())
    {
        return std::unexpected(calculated.error());
    }
    if (calculated->writes.empty())
    {
        return NumericEditOutcome{NumericEditUnchanged{.reason = calculated->no_change}};
    }
    const auto written = write_patch(*session, spec, run->cells.size(), calculated->writes);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return NumericEditOutcome{NumericEditChanged{}};
}

} // namespace fastecu::calibration
