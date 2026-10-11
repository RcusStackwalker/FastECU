#include "src/backend/calibration/map_edit.h"

#include <limits>

#include "src/backend/calibration/scaling_internal.h"

namespace fastecu::calibration
{
using namespace internal;

std::uint64_t ElementByteAddress(const MapElementSpec& spec, std::uint32_t index)
{
    const std::uint32_t width = definition::StorageByteSize(spec.storage_type);

    // Layout matches decode_numeric_run (calibration_service.cpp) exactly --
    // spec's defect (b): the edit path used to lay elements out flat
    // (address + index*width), ignoring start_position/interval, which
    // decode_numeric_run honours. addr(j) = address + (start_position-1) *
    // width + j * width * interval.
    //
    // start_position is 1-based. definition_resolver rejects 0, but clamp
    // identically to decode_numeric_run' guard so the two paths cannot
    // disagree if this is ever reached with an out-of-domain 0 directly.
    const std::uint64_t start_offset = spec.start_position == 0 ? 0 : std::uint64_t(spec.start_position - 1);

    // Overflow-checked exactly as decode_numeric_run: on overflow, return a
    // sentinel address a real ROM can never contain rather than wrapping, so a
    // caller's bounds check rejects it instead of silently targeting a
    // wrapped-around address.
    constexpr std::uint64_t kOverflowSentinel = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t start_byte_offset = 0;
    std::uint64_t stride = 0;
    std::uint64_t element_offset = 0;
    std::uint64_t address = 0;
    if (!CheckedMultiply(start_offset, width, start_byte_offset) || !CheckedMultiply(width, spec.interval, stride) ||
        !CheckedMultiply(std::uint64_t(index), stride, element_offset) ||
        !CheckedAdd(spec.address, start_byte_offset, address) || !CheckedAdd(address, element_offset, address))
    {
        return kOverflowSentinel;
    }

    return address;
}

Result<std::vector<std::uint8_t>> WriteRawElement(const MapElementSpec& spec, std::int64_t raw)
{
    const std::uint32_t width = definition::StorageByteSize(spec.storage_type);
    const bool is_float = spec.storage_type == definition::StorageType::kFloat;

    // `raw`'s low 32 bits are packed bit-for-bit -- for float storage this is
    // already the encoded float's bit pattern, not a number to convert; see
    // this function's doc comment.
    const std::uint32_t packed = static_cast<std::uint32_t>(raw);

    std::vector<std::uint8_t> out(width, 0x00);
    const bool little_endian = !is_float && (spec.endian == "little");
    for (std::uint32_t k = 0; k < width; ++k)
    {
        const std::uint32_t shift = little_endian ? (8U * k) : (8U * (width - 1U - k));
        out[k] = static_cast<std::uint8_t>((packed >> shift) & 0xFFU);
    }
    return out;
}

EditTarget ResolveEditTarget(const SelectionRange& selection, MapDimensions dims, std::string_view x_scale_type)
{
    // Ported from inc_dec_value's three-way branch (menu_actions.cpp), the
    // canonical copy duplicated verbatim across inc_dec_value, set_value, and
    // interpolate_value. See this function's header comment for the rule.
    int first_col = selection.first_col - 1;
    int first_row = selection.first_row - 1;
    int last_col = selection.last_col - 1;
    int last_row = selection.last_row - 1;

    std::uint32_t x_size = dims.x_size;

    const bool is_static_scale = x_scale_type == "Static Y Axis" || x_scale_type == "Static X Axis";

    if (selection.first_col == 0 && dims.y_size > 1)
    {
        if (is_static_scale)
        {
            return {.kind = EditTargetKind::kRejected, .range = {}, .x_size = dims.x_size};
        }
        first_col++;
        last_col++;
        x_size = 1;
        return {.kind = EditTargetKind::kYAxis,
                .range = {.first_row = first_row, .first_col = first_col, .last_row = last_row, .last_col = last_col},
                .x_size = x_size};
    }

    if (selection.first_row == 0 && dims.x_size > 1)
    {
        if (is_static_scale)
        {
            return {.kind = EditTargetKind::kRejected, .range = {}, .x_size = dims.x_size};
        }
        first_row++;
        last_row++;
        // A single-row map has no Y-axis header column to skip.
        if (dims.y_size == 1)
        {
            first_col++;
            last_col++;
        }
        return {.kind = EditTargetKind::kXAxis,
                .range = {.first_row = first_row, .first_col = first_col, .last_row = last_row, .last_col = last_col},
                .x_size = x_size};
    }

    if (dims.x_size == 1 && !is_static_scale)
    {
        first_row++;
        last_row++;
    }
    if (dims.y_size == 1)
    {
        first_col++;
        last_col++;
    }

    return {.kind = EditTargetKind::kMapBody,
            .range = {.first_row = first_row, .first_col = first_col, .last_row = last_row, .last_col = last_col},
            .x_size = x_size};
}

} // namespace fastecu::calibration
