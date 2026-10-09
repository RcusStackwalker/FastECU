#include "src/backend/calibration/numeric_map_edit.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <optional>

#include "src/algorithms/expression/checked_expression.h"
#include "src/backend/calibration/scaling_internal.h"

namespace fastecu::calibration
{
namespace
{
struct Limits
{
    std::optional<double> minimum;
    std::optional<double> maximum;
};

Result<std::optional<double>> parse_limit(std::string_view text)
{
    if (text.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos)
    {
        return std::optional<double>{};
    }
    const auto value = expression::ParseFiniteNumber(text);
    if (!value.has_value())
    {
        return fail(ErrorKind::kInvalidConfig, std::format("invalid definition limit: {}", value.error().detail));
    }
    return std::optional<double>(*value);
}

Result<Limits> limits_for(const MapElementSpec& spec)
{
    const auto minimum = parse_limit(spec.min_value);
    if (!minimum.has_value())
    {
        return std::unexpected(minimum.error());
    }
    const auto maximum = parse_limit(spec.max_value);
    if (!maximum.has_value())
    {
        return std::unexpected(maximum.error());
    }
    if (minimum->has_value() && maximum->has_value() && **minimum > **maximum)
    {
        return fail(ErrorKind::kInvalidConfig, "definition minimum exceeds maximum");
    }
    return Limits{*minimum, *maximum};
}

Status validate_selection(std::uint32_t width, std::uint64_t count, const SelectionRange& range)
{
    if (width == 0 || count == 0 || count > std::numeric_limits<std::uint32_t>::max() || count % width != 0 ||
        range.first_row < 0 || range.first_col < 0 || range.last_row < range.first_row ||
        range.last_col < range.first_col || static_cast<std::uint64_t>(range.last_col) >= width ||
        static_cast<std::uint64_t>(range.last_row) >= count / width)
    {
        return fail(ErrorKind::kInvalidConfig, "edit selection exceeds its numeric run");
    }
    return {};
}

Result<bytes::Bytes> encode_value(const MapElementSpec& spec, double value)
{
    if (!spec.storage_type.has_value() || spec.storage_type == definition::StorageType::kBloblist ||
        spec.start_position == 0 || spec.interval == 0 || !std::isfinite(value))
    {
        return fail(ErrorKind::kInvalidConfig, "edit has invalid numeric storage, stride, or value");
    }
    const auto encoded = expression::EvaluateChecked(spec.to_byte, value);
    if (!encoded.has_value())
    {
        return fail(ErrorKind::kInvalidConfig, std::format("encoding expression: {}", encoded.error().detail));
    }
    if (spec.storage_type == definition::StorageType::kFloat)
    {
        if (std::abs(*encoded) > static_cast<double>(std::numeric_limits<float>::max()))
        {
            return fail(ErrorKind::kInvalidConfig, "value exceeds finite float storage range");
        }
        const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(*encoded));
        return write_raw_element(spec, static_cast<std::int64_t>(bits));
    }
    const auto bit_count = definition::storage_byte_size(spec.storage_type) * 8U;
    const bool is_unsigned = definition::is_unsigned_storage(spec.storage_type);
    const double minimum = is_unsigned ? 0.0 : -static_cast<double>(std::uint64_t{1} << (bit_count - 1U));
    const double maximum = static_cast<double>((std::uint64_t{1} << (is_unsigned ? bit_count : bit_count - 1U)) - 1U);
    const double rounded = std::round(*encoded);
    if (rounded < minimum || rounded > maximum)
    {
        return fail(ErrorKind::kInvalidConfig,
                    std::format("encoded value is outside storage range [{}, {}]", minimum, maximum));
    }
    return write_raw_element(spec, static_cast<std::int64_t>(rounded));
}

void merge_reason(NoChangeReason& previous, NoChangeReason next)
{
    if (next == NoChangeReason::kUnchanged)
    {
        return;
    }
    if (previous == NoChangeReason::kUnchanged)
    {
        previous = next;
    }
    else if (previous != next)
    {
        previous = NoChangeReason::kMultipleCauses;
    }
}

Status append_write(NumericEditResult& result, bytes::ByteView rom, const MapElementSpec& spec, const Limits& limits,
                    std::uint32_t index, double requested, const NumericCell *old_value)
{
    if (!std::isfinite(requested))
    {
        return fail(ErrorKind::kInvalidConfig, "requested value is not finite");
    }
    double clamped = requested;
    if (limits.minimum.has_value())
    {
        clamped = std::max(clamped, *limits.minimum);
    }
    if (limits.maximum.has_value())
    {
        clamped = std::min(clamped, *limits.maximum);
    }
    auto encoded = encode_value(spec, clamped);
    if (!encoded.has_value())
    {
        return std::unexpected(encoded.error());
    }
    const auto address = element_byte_address(spec, index, true);
    if (!internal::byte_window_fits(rom, address, encoded->size()))
    {
        return fail(ErrorKind::kInvalidConfig, "edit byte range exceeds ROM size");
    }
    if (std::ranges::equal(rom.subspan(static_cast<std::size_t>(address), encoded->size()), *encoded))
    {
        const auto reason = clamped != requested ? NoChangeReason::kDefinitionLimit
                            : old_value != nullptr && old_value->has_value() && **old_value != clamped
                                ? NoChangeReason::kBelowStorageResolution
                                : NoChangeReason::kUnchanged;
        merge_reason(result.no_change, reason);
    }
    else
    {
        result.writes.push_back({.index = index, .byte_address = address, .bytes = std::move(*encoded)});
    }
    return {};
}

template <class Candidate>
Result<NumericEditResult> build_patch(bytes::ByteView rom, const MapElementSpec& spec, std::uint32_t width,
                                      std::span<const NumericCell> cells, const SelectionRange& range,
                                      Candidate candidate)
{
    const auto valid = validate_selection(width, cells.size(), range);
    if (!valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto limits = limits_for(spec);
    if (!limits.has_value())
    {
        return std::unexpected(limits.error());
    }
    NumericEditResult result{.no_change = NoChangeReason::kUnchanged};
    for (std::uint64_t row = static_cast<std::uint64_t>(range.first_row);
         row <= static_cast<std::uint64_t>(range.last_row); ++row)
    {
        for (std::uint64_t col = static_cast<std::uint64_t>(range.first_col);
             col <= static_cast<std::uint64_t>(range.last_col); ++col)
        {
            const auto index = static_cast<std::uint32_t>(row * width + col);
            const auto value = candidate(index, row, col);
            if (!value.has_value())
            {
                return fail(value.error().kind, std::format("cell {}: {}", index, value.error().detail));
            }
            const auto appended = append_write(result, rom, spec, *limits, index, *value, &cells[index]);
            if (!appended.has_value())
            {
                return fail(appended.error().kind, std::format("cell {}: {}", index, appended.error().detail));
            }
        }
    }
    if (!result.writes.empty())
    {
        result.no_change = NoChangeReason::kNone;
    }
    return result;
}

Result<double> current_value(std::span<const NumericCell> cells, std::uint32_t index)
{
    if (!cells[index].has_value())
    {
        return std::unexpected(cells[index].error());
    }
    if (!std::isfinite(*cells[index]))
    {
        return fail(ErrorKind::kInvalidConfig, "current cell is not finite");
    }
    return cells[index];
}

Result<bool> uses_current_value(std::string_view formula)
{
    if (formula.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos)
    {
        return true;
    }
    bool uses_input = false;
    while (!formula.empty())
    {
        const char token = formula.front();
        if (token == 'x')
        {
            uses_input = true;
            formula.remove_prefix(1);
        }
        else if (token == '.' || (token >= '0' && token <= '9'))
        {
            double number = 0.0;
            const auto parsed = std::from_chars(formula.data(), formula.data() + formula.size(), number);
            if (parsed.ec != std::errc{} || !std::isfinite(number))
            {
                return fail(ErrorKind::kInvalidConfig, "assignment contains an invalid numeric literal");
            }
            formula.remove_prefix(static_cast<std::size_t>(parsed.ptr - formula.data()));
        }
        else if (std::string_view(" \t\r\n\f\v+-*/()").find(token) != std::string_view::npos)
        {
            formula.remove_prefix(1);
        }
        else
        {
            return fail(ErrorKind::kInvalidConfig, "assignment contains an unsupported token");
        }
    }
    return uses_input;
}

Result<double> interpolate_pair(std::span<const NumericCell> cells, std::uint64_t first, std::uint64_t last,
                                double fraction)
{
    const auto left = current_value(cells, static_cast<std::uint32_t>(first));
    if (!left.has_value())
    {
        return left;
    }
    const auto right = current_value(cells, static_cast<std::uint32_t>(last));
    if (!right.has_value())
    {
        return right;
    }
    return std::lerp(*left, *right, fraction);
}

Result<double> interpolated(std::span<const NumericCell> cells, std::uint32_t width, const SelectionRange& range,
                            InterpolationMode mode, std::uint64_t row, std::uint64_t col)
{
    const auto first_row = static_cast<std::uint64_t>(range.first_row);
    const auto last_row = static_cast<std::uint64_t>(range.last_row);
    const auto first_col = static_cast<std::uint64_t>(range.first_col);
    const auto last_col = static_cast<std::uint64_t>(range.last_col);
    const double horizontal_fraction =
        first_col == last_col ? 0.0 : static_cast<double>(col - first_col) / static_cast<double>(last_col - first_col);
    const double vertical_fraction =
        first_row == last_row ? 0.0 : static_cast<double>(row - first_row) / static_cast<double>(last_row - first_row);
    if (mode == InterpolationMode::kHorizontal)
    {
        return interpolate_pair(cells, row * width + first_col, row * width + last_col, horizontal_fraction);
    }
    if (mode == InterpolationMode::kVertical)
    {
        return interpolate_pair(cells, first_row * width + col, last_row * width + col, vertical_fraction);
    }
    const auto top =
        interpolate_pair(cells, first_row * width + first_col, first_row * width + last_col, horizontal_fraction);
    if (!top.has_value())
    {
        return top;
    }
    const auto bottom =
        interpolate_pair(cells, last_row * width + first_col, last_row * width + last_col, horizontal_fraction);
    if (!bottom.has_value())
    {
        return bottom;
    }
    return std::lerp(*top, *bottom, vertical_fraction);
}

} // namespace

Result<NumericEditResult> calculate_increment(bytes::ByteView rom, const MapElementSpec& spec, std::uint32_t width,
                                              std::span<const NumericCell> cells, const SelectionRange& range,
                                              IncrementStep step)
{
    const bool fine = step == IncrementStep::kFineUp || step == IncrementStep::kFineDown;
    const bool down = step == IncrementStep::kFineDown || step == IncrementStep::kCoarseDown;
    const double amount = (fine ? spec.fine_increment : spec.coarse_increment) * (down ? -1.0 : 1.0);
    if (!std::isfinite(amount) || amount == 0.0)
    {
        return fail(ErrorKind::kInvalidConfig, "increment must be finite and nonzero");
    }
    return build_patch(rom, spec, width, cells, range,
                       [&](std::uint32_t index, std::uint64_t, std::uint64_t) -> Result<double>
                       {
                           const auto current = current_value(cells, index);
                           if (!current.has_value())
                           {
                               return current;
                           }
                           return *current + amount;
                       });
}

Result<NumericEditResult> calculate_assignment(bytes::ByteView rom, const MapElementSpec& spec, std::uint32_t width,
                                               std::span<const NumericCell> cells, const SelectionRange& range,
                                               std::string_view formula)
{
    const auto needs_input = uses_current_value(formula);
    if (!needs_input.has_value())
    {
        return std::unexpected(needs_input.error());
    }
    return build_patch(rom, spec, width, cells, range,
                       [&](std::uint32_t index, std::uint64_t, std::uint64_t) -> Result<double>
                       {
                           const auto current = *needs_input ? current_value(cells, index) : Result<double>(0.0);
                           if (!current.has_value())
                           {
                               return current;
                           }
                           const auto value = expression::EvaluateChecked(formula, *current);
                           if (!value.has_value())
                           {
                               return fail(ErrorKind::kInvalidConfig, value.error().detail);
                           }
                           return *value;
                       });
}

Result<NumericEditResult> calculate_interpolation(bytes::ByteView rom, const MapElementSpec& spec, std::uint32_t width,
                                                  std::span<const NumericCell> cells, const SelectionRange& range,
                                                  InterpolationMode mode)
{
    return build_patch(rom, spec, width, cells, range, [&](std::uint32_t, std::uint64_t row, std::uint64_t col)
                       { return interpolated(cells, width, range, mode, row, col); });
}

Result<NumericEditResult> calculate_paste(bytes::ByteView rom, const MapElementSpec& spec, std::uint32_t width,
                                          std::uint32_t height, const SelectionRange& range,
                                          std::span<const std::vector<double>> values)
{
    const auto valid = validate_selection(width, std::uint64_t(width) * height, range);
    if (!valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto limits = limits_for(spec);
    if (!limits.has_value())
    {
        return std::unexpected(limits.error());
    }
    NumericEditResult result{.no_change = NoChangeReason::kUnchanged};
    const auto columns = values.empty() ? 0 : values.front().size();
    const auto rows_to_use = std::min(values.size(), std::size_t(height) - static_cast<std::size_t>(range.first_row));
    for (std::size_t row = 0; row < rows_to_use; ++row)
    {
        const auto columns_to_use =
            std::min({columns, values[row].size(), std::size_t(width) - static_cast<std::size_t>(range.first_col)});
        for (std::size_t col = 0; col < columns_to_use; ++col)
        {
            const auto index = static_cast<std::uint32_t>((static_cast<std::size_t>(range.first_row) + row) * width +
                                                          static_cast<std::size_t>(range.first_col) + col);
            const auto appended = append_write(result, rom, spec, *limits, index, values[row][col], nullptr);
            if (!appended.has_value())
            {
                return fail(appended.error().kind, std::format("cell {}: {}", index, appended.error().detail));
            }
        }
    }
    if (!result.writes.empty())
    {
        result.no_change = NoChangeReason::kNone;
    }
    return result;
}
} // namespace fastecu::calibration
