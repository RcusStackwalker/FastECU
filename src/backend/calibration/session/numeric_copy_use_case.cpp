#include "src/backend/calibration/session/numeric_copy_use_case.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace fastecu::calibration
{
namespace
{

NumericCopyOutcome NotApplicable(NotApplicableReason reason)
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
std::int64_t RunWidth(const MapDimensions& map, NumericTarget target)
{
    return target == NumericTarget::kYAxis ? 1 : map.x_size;
}

// An X axis is one row; a body or Y axis is as tall as the map.
std::int64_t RunHeight(const MapDimensions& map, NumericTarget target)
{
    return target == NumericTarget::kXAxis ? 1 : map.y_size;
}

// Shortest decimal that parses back to the same double. Fixed notation keeps
// the text acceptable to the dot-decimal paste parser.
std::string FormatValue(double value)
{
    std::array<char, 400> buffer{};
    const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed);
    return std::string(buffer.data(), written.ptr);
}

} // namespace

Result<NumericCopyOutcome> CopyNumericValues(CalibrationWorkspace& workspace, const NumericCopyRequest& request)
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
    const auto& maps = session->Definition()->definition.maps;
    if (request.map_index >= maps.size())
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
    const auto& map = maps[request.map_index];
    const MapDimensions dimensions{.x_size = map.x_size, .y_size = map.y_size};
    const auto width = RunWidth(dimensions, request.selection.target);
    const auto height = RunHeight(dimensions, request.selection.target);
    const auto& range = request.selection.elements;
    if (range.first_row < 0 || range.first_col < 0 || range.first_row > range.last_row ||
        range.first_col > range.last_col || range.last_row >= height || range.last_col >= width ||
        static_cast<std::int64_t>(run->cells.size()) != width * height)
    {
        return Fail(ErrorKind::kInvalidConfig, "copy selection lies outside the target run");
    }

    std::string text;
    for (int row = range.first_row; row <= range.last_row; ++row)
    {
        if (row != range.first_row)
        {
            text += '\n';
        }
        for (int col = range.first_col; col <= range.last_col; ++col)
        {
            if (col != range.first_col)
            {
                text += '\t';
            }
            const auto& cell = run->cells[static_cast<std::size_t>(row * width + col)];
            if (!cell.has_value())
            {
                return NumericCopyOutcome{
                    NumericCopyInvalidCell{.row = row, .col = col, .detail = cell.error().detail}};
            }
            text += FormatValue(*cell);
        }
    }
    return NumericCopyOutcome{NumericCopyText{.text = std::move(text)}};
}

} // namespace fastecu::calibration
