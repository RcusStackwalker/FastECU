#include "src/backend/calibration/map_edit.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <format>

#include "src/algorithms/expression/expression_evaluator.h"
#include "src/backend/calibration/scaling_internal.h"

namespace fastecu::calibration
{
using namespace internal;

std::uint64_t element_byte_address(const MapElementSpec& spec, std::uint32_t index, bool for_write)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);
    std::uint64_t address = spec.address + std::uint64_t(index) * width;

    // Legacy applies two DIFFERENT wrx02 relocation predicates on the read and
    // write paths. Preserved verbatim and kept visibly side by side; the spec's
    // defect (a) covers the divergence and 6b-4 reconciles them.
    if (spec.flash_method != "wrx02")
    {
        return address;
    }
    constexpr std::uint64_t kSizeThreshold = std::uint64_t(190) * 1024;
    const bool relocate =
        for_write ? (spec.rom_file_size < kSizeThreshold && address > 0x27FFF) : (spec.rom_file_size < address);
    return relocate ? address - 0x8000 : address;
}

Result<std::int64_t> read_raw_element(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t index)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);
    const std::uint64_t address = element_byte_address(spec, index, /*for_write=*/false);

    if (!byte_window_fits(rom_data, address, width))
    {
        return fail(ErrorKind::Internal,
                    std::format("element {} at 0x{:x} runs past ROM size {}", index, address, rom_data.size()));
    }

    // Ported from get_rom_data_value (menu_actions.cpp:1610-1697). Legacy
    // assembles two representations from the same window: `data_byte`, a
    // correctly endian-aware unsigned assembly used for uint reads below,
    // and `byte_value`, filled most-significant-byte-first in BOTH endian
    // branches -- legacy's bug, preserved here verbatim (see the spec's
    // defect (b), pinned by PinnedDefect_SignedMultiByteReadsAreByteSwapped
    // in map_edit_test.cpp).
    std::uint32_t data_byte = 0;
    std::array<std::uint8_t, 4> byte_value{};

    const bool little_or_float = spec.endian == "little" || spec.storage_type == definition::StorageType::Float;

    for (std::uint32_t k = 0; k < width; ++k)
    {
        const std::uint64_t src = little_or_float ? address + width - 1 - k : address + k;
        const std::uint8_t raw_byte = rom_data[static_cast<std::size_t>(src)];
        data_byte = (data_byte << 8) + raw_byte;
        byte_value[k] = raw_byte;
    }

    if (definition::is_unsigned_storage(spec.storage_type))
    {
        return static_cast<std::int64_t>(data_byte);
    }

    if (spec.storage_type == definition::StorageType::Float)
    {
        // The one intentional change from legacy: assemble the four bytes
        // into a uint32_t and std::bit_cast to float, rather than reading a
        // union member that was never written (UB). The float bits are then
        // bit_cast back to int32_t so they round-trip through this
        // function's int64_t return type unchanged.
        //
        // Legacy reads map_data_value.float_value out of a union whose
        // float member overlaps the same byte_value[4] used above. On a
        // little-endian host that union member's least-significant byte is
        // byte_value[0] -- the same LSB-first layout the Int32 branch below
        // already uses on this identically-filled array. Since byte_value
        // was itself filled from address+width-1 down to address+0 (see the
        // little_or_float branch above, always taken for float storage),
        // this makes address+0 the float's most-significant byte: a
        // big-endian float in ROM, matching decode_scaled_values's
        // documented float handling in calibration_service.cpp.
        const std::uint32_t bits = std::uint32_t(byte_value[0]) | (std::uint32_t(byte_value[1]) << 8) |
                                   (std::uint32_t(byte_value[2]) << 16) | (std::uint32_t(byte_value[3]) << 24);
        const float float_value = std::bit_cast<float>(bits);
        return static_cast<std::int64_t>(std::bit_cast<std::int32_t>(float_value));
    }

    // Signed integer storage. Legacy reads these back out of the same
    // byte_value bytes through a union of int8_t/int16_t/int32_t members
    // sharing storage with the uint8_t byte_value[4] used above.
    // Recombined here in that union member's little-endian-host layout
    // (index 0 = least significant byte of the *_value member), which,
    // combined with byte_value being filled MSB-first above, reproduces
    // the byte swap the spec's defect (b) pins.
    if (width == 1)
    {
        return static_cast<std::int64_t>(sign_extend(byte_value[0], 1));
    }
    if (width == 2)
    {
        const std::uint32_t raw = std::uint32_t(byte_value[0]) | (std::uint32_t(byte_value[1]) << 8);
        return static_cast<std::int64_t>(sign_extend(raw, 2));
    }
    if (width == 4)
    {
        const std::uint32_t raw = std::uint32_t(byte_value[0]) | (std::uint32_t(byte_value[1]) << 8) |
                                  (std::uint32_t(byte_value[2]) << 16) | (std::uint32_t(byte_value[3]) << 24);
        return static_cast<std::int64_t>(sign_extend(raw, 4));
    }

    // width == 3: int24. Legacy's signed branch tests only storagesize 1, 2,
    // and 4, so a 3-byte signed value falls through every branch and the
    // QString `value` is never assigned -- an empty string, which callers
    // convert to 0. This is legacy behavior being preserved, not an
    // oversight in this port -- see PinnedDefect_Int24AlwaysReadsAsZero.
    return 0;
}

Result<std::vector<std::uint8_t>> write_raw_element(const MapElementSpec& spec, std::int64_t raw,
                                                    bool legacy_byte_order)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);
    const bool is_float = spec.storage_type == definition::StorageType::Float;

    // `raw`'s low 32 bits are packed bit-for-bit -- for float storage this is
    // already the encoded float's bit pattern, not a number to convert; see
    // this function's doc comment.
    const std::uint32_t packed = static_cast<std::uint32_t>(raw);

    std::vector<std::uint8_t> out(width, 0x00);
    const bool little_endian = !is_float && ((spec.endian == "little") != legacy_byte_order);
    for (std::uint32_t k = 0; k < width; ++k)
    {
        const std::uint32_t shift = little_endian ? (8U * k) : (8U * (width - 1U - k));
        out[k] = static_cast<std::uint8_t>((packed >> shift) & 0xFFU);
    }
    return out;
}

EditTarget resolve_edit_target(const SelectionRange& selection, MapDimensions dims, std::string_view x_scale_type)
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
            return {.kind = EditTargetKind::Rejected, .range = {}, .x_size = dims.x_size};
        }
        first_col++;
        last_col++;
        x_size = 1;
        return {.kind = EditTargetKind::YAxis,
                .range = {.first_row = first_row, .first_col = first_col, .last_row = last_row, .last_col = last_col},
                .x_size = x_size};
    }

    if (selection.first_row == 0 && dims.x_size > 1)
    {
        if (is_static_scale)
        {
            return {.kind = EditTargetKind::Rejected, .range = {}, .x_size = dims.x_size};
        }
        first_row++;
        last_row++;
        return {.kind = EditTargetKind::XAxis,
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

    return {.kind = EditTargetKind::MapBody,
            .range = {.first_row = first_row, .first_col = first_col, .last_row = last_row, .last_col = last_col},
            .x_size = x_size};
}

Result<std::vector<std::uint8_t>> encode_scaled_value(const MapElementSpec& spec, double display_value,
                                                      int float_precision, bool legacy_byte_order)
{
    const bool is_float = spec.storage_type == definition::StorageType::Float;

    const double encoded =
        expression_evaluate(spec.to_byte, format_like_qt_g(display_value, float_precision), float_precision);

    std::int64_t raw = 0;
    if (is_float)
    {
        raw = static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(static_cast<float>(encoded)));
    }
    else
    {
        raw = static_cast<std::int64_t>(static_cast<std::uint32_t>(std::llround(encoded)));
    }

    return write_raw_element(spec, raw, legacy_byte_order);
}

int map_value_decimal_count(std::string_view value_format)
{
    // Ported from get_mapvalue_decimal_count (menu_actions.cpp). Legacy's
    // QString::split(".").at(1) is the segment between the FIRST and SECOND
    // '.', not everything after the first '.' -- reproduced here by
    // searching for a second '.' and bounding the counted segment at it.
    const std::size_t first_dot = value_format.find('.');
    if (first_dot == std::string_view::npos)
    {
        return 0;
    }

    const std::size_t second_dot = value_format.find('.', first_dot + 1);
    const std::string_view segment = value_format.substr(
        first_dot + 1, second_dot == std::string_view::npos ? std::string_view::npos : second_dot - (first_dot + 1));

    return static_cast<int>(std::count(segment.begin(), segment.end(), '0'));
}

double map_cell_color_scale(double value, double min_value, double max_value)
{
    // Ported from get_map_cell_colors (menu_actions.cpp). min_value ==
    // max_value divides by zero, producing a non-finite hue -- preserved
    // verbatim; see PinnedDefect_EqualColorBoundsProduceNonFiniteHue.
    constexpr double kScaleStart = 210.0 / 360.0;
    const double color_scale = (1.0 - (value - min_value) / (max_value - min_value)) * kScaleStart;
    double color_value = kScaleStart - color_scale;

    if (color_value < 0.0)
    {
        color_value = 0.0;
    }

    return color_value;
}

} // namespace fastecu::calibration
