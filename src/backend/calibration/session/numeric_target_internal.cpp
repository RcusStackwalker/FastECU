#include "src/backend/calibration/session/numeric_target_internal.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/algorithms/expression/expression.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/calibration/definition_bytes.h"

namespace fastecu::calibration::internal
{
namespace
{

NumericRun *TargetCells(DecodedMap& values, NumericTarget target)
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

// Shortest decimal that parses back to the same double. Fixed notation keeps
// the text acceptable to the dot-decimal paste parser.
std::string FormatValue(double value)
{
    std::array<char, 400> buffer{};
    const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed);
    return std::string(buffer.data(), written.ptr);
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

Result<NumericEditResult> Calculate(const DefinitionBytes& rom, const MapElementSpec& spec, const SelectionRange& range,
                                    std::uint32_t width, std::uint32_t height, std::span<const NumericCell> cells,
                                    const NumericEditOperation& operation)
{
    return std::visit(
        [&](const auto& edit) -> Result<NumericEditResult>
        {
            using Edit = std::decay_t<decltype(edit)>;
            if constexpr (std::is_same_v<Edit, IncrementEdit>)
            {
                return CalculateIncrement(rom, spec, width, cells, range, edit.step);
            }
            else if constexpr (std::is_same_v<Edit, AssignmentEdit>)
            {
                return CalculateAssignment(rom, spec, width, cells, range, edit.expression);
            }
            else if constexpr (std::is_same_v<Edit, InterpolationEdit>)
            {
                return CalculateInterpolation(rom, spec, width, cells, range, edit.mode);
            }
            else
            {
                static_assert(std::is_same_v<Edit, PasteEdit>);
                const auto values = ParsePaste(edit);
                if (!values.has_value())
                {
                    return std::unexpected(values.error());
                }
                return CalculatePaste(rom, spec, width, height, range, *values);
            }
        },
        operation);
}

// Validates every write against the target run and the session's write rules
// before any byte changes, so one bad cell rejects the whole edit.
// Byte-identical cells never reach the patch: AppendWrite leaves them out.
Status WritePatch(CalibrationSession& session, const MapElementSpec& spec, std::size_t cell_count,
                  const NumericEditPatch& patch)
{
    const auto width = definition::StorageByteSize(spec.storage_type);
    for (const auto& cell : patch)
    {
        if (cell.index >= cell_count || cell.bytes.size() != width ||
            cell.byte_address != ElementByteAddress(spec, cell.index))
        {
            return Fail(ErrorKind::kInvalidConfig, "map edit index, address, or byte width does not match its target");
        }
        if (const Status writable = session.CheckWrite(cell.byte_address, cell.bytes.size()); !writable.has_value())
        {
            return writable;
        }
    }
    for (const auto& cell : patch)
    {
        const auto written = session.WriteBytes(cell.byte_address, cell.bytes);
        if (!written.has_value())
        {
            return written;
        }
    }
    return {};
}

} // namespace

PreparedNumericTarget::PreparedNumericTarget(CalibrationSession& session, ResolvedElement element, NumericRun run)
    : element_(std::move(element)), run_(std::move(run)), session_(&session)
{
}

NumericTargetPreparation PrepareNumericTarget(CalibrationWorkspace& workspace, SessionId session_id,
                                              std::size_t map_index, NumericTarget target)
{
    CalibrationSession *session = workspace.Find(session_id);
    if (session == nullptr)
    {
        return NumericEditNotApplicable{.reason = NotApplicableReason::kClosedSession};
    }
    if (session->Definition() == nullptr)
    {
        return NumericEditNotApplicable{.reason = NotApplicableReason::kNoDefinition};
    }
    const auto& definition = session->Definition()->definition;
    if (map_index >= definition.maps.size())
    {
        return NumericEditNotApplicable{.reason = NotApplicableReason::kUnavailableTarget};
    }
    auto decoded = session->DecodeMap(map_index);
    if (!decoded.has_value())
    {
        return NumericEditNotApplicable{.reason = NotApplicableReason::kUnavailableTarget};
    }
    NumericRun *run = TargetCells(*decoded, target);
    if (run == nullptr)
    {
        return NumericEditNotApplicable{.reason = NotApplicableReason::kUnavailableTarget};
    }
    return PreparedNumericTarget(*session, ResolveMapElement(definition, definition.maps[map_index], target),
                                 std::move(*run));
}

Result<NumericCopyOutcome> PreparedNumericTarget::Copy(const SelectionRange& range) const
{
    const std::int64_t width = element_.width;
    const std::int64_t height = element_.height;
    if (range.first_row < 0 || range.first_col < 0 || range.first_row > range.last_row ||
        range.first_col > range.last_col || range.last_row >= height || range.last_col >= width ||
        static_cast<std::int64_t>(run_.cells.size()) != width * height)
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
            const auto& cell = run_.cells[static_cast<std::size_t>(row * width + col)];
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

Result<NumericEditOutcome> PreparedNumericTarget::Apply(const SelectionRange& range,
                                                        const NumericEditOperation& operation)
{
    const auto spec = element_.EditSpec();
    // The target run's bytes, rendered from the session's memory image.
    const std::uint64_t end = ElementRunEnd(spec.address, spec.start_position, spec.interval, element_.byte_width,
                                            static_cast<std::uint32_t>(run_.cells.size()));
    const auto window = session_->Read(spec.address, static_cast<std::size_t>(end - spec.address));
    if (!window.has_value())
    {
        return std::unexpected(window.error());
    }
    const auto calculated = Calculate(DefinitionBytes(*window, spec.address), spec, range, element_.width,
                                      element_.height, run_.cells, operation);
    if (!calculated.has_value())
    {
        return std::unexpected(calculated.error());
    }
    if (calculated->writes.empty())
    {
        return NumericEditOutcome{NumericEditUnchanged{.reason = calculated->no_change}};
    }
    const auto written = WritePatch(*session_, spec, run_.cells.size(), calculated->writes);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return NumericEditOutcome{NumericEditChanged{}};
}

} // namespace fastecu::calibration::internal
