#include "src/backend/calibration/map_edit.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>

#include "src/algorithms/expression/expression_evaluator.h"
#include "src/backend/calibration/scaling_internal.h"

namespace fastecu::calibration
{
using namespace internal;

namespace
{

// Mirrors QString::toFloat()'s default-0-on-failure behavior (the `ok`
// pointer is never checked at any inc_dec_value call site this ports): the
// whole string must be a valid float or the parse yields 0.0. Every text this
// sees is machine-generated decimal (cell text from a prior read, or this
// file's own format_like_qt_g output), never a hand-typed user string, so
// this does not need to replicate every QString::toFloat() edge case
// (locale, grouping, stray whitespace).
float to_float_or_zero(std::string_view text)
{
    float value = 0.0F;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return 0.0F;
    }
    return value;
}

// Mirrors QString::toUInt(): the whole string must be a valid, non-negative
// decimal integer or the parse yields 0 (the `ok` pointer is never checked at
// any call site this ports). Qt's `uint` is always 32 bits regardless of
// host, which matters here: the uint32 saturation check below
// (`new_rom_data_value.toUInt() > 0xffffffff`) is legacy dead code that can
// never fire only because toUInt()'s result can never exceed 0xFFFFFFFF --
// preserved by parsing into the same 32-bit width, not a wider one.
std::uint32_t to_uint32_or_zero(std::string_view text)
{
    if (text.empty() || text.front() == '-')
    {
        return 0;
    }
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return 0;
    }
    return value;
}

// Mirrors QString::toInt() (qint32, whole-string-or-0), same rationale as
// to_uint32_or_zero above.
std::int32_t to_int32_or_zero(std::string_view text)
{
    std::int32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return 0;
    }
    return value;
}

// The inverse of format_raw_value_display below: converts a to_byte-encoded
// text value into the raw form write_raw_element expects. Mirrors
// fastecu::ui::raw_element_value_from_text (map_edit_adapter.cpp), the
// Qt-layer equivalent of this function, in pure C++ so the portable calibration
// package can produce CellPatch::bytes without depending on Qt.
std::int64_t raw_from_display_text(const MapElementSpec& spec, std::string_view text)
{
    if (spec.storage_type == definition::StorageType::Float)
    {
        // text is a decimal string of the float VALUE (matches how
        // read/format work throughout this file) -- convert via the actual
        // numeric value, then bit_cast to get the IEEE-754 bit pattern, NOT
        // parsed as an integer.
        float value = 0.0F;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(value));
    }
    // Every other storage type: parse as a plain integer. Mirrors
    // QString::toInt()'s whole-string-must-parse-or-0 semantics -- the
    // inputs this function actually receives are always machine-generated
    // integer strings, so a simple whole-string parse with a 0 fallback on
    // any failure is sufficient.
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return 0;
    }
    return value;
}

// Formats a raw stored value for display, mirroring legacy get_rom_data_value
// (via fastecu::ui::format_raw_element_value, the Qt-layer counterpart in
// map_edit_adapter.cpp) -- with one deliberate difference: the float branch
// there is `QString::number(bit_cast<float>(raw))`, a BARE QString::number
// call (float implicitly promotes to double), which formats with Qt's
// default 'g' precision 6, not `float_precision`. Reproduced here with
// format_like_qt_g(value, 6) accordingly -- see apply_increment's doc comment
// for why this file uses precision 6 (not float_precision) at every such bare
// call site.
std::string format_raw_value_display(const MapElementSpec& spec, std::int64_t raw)
{
    if (!spec.storage_type.has_value())
    {
        return {};
    }
    if (spec.storage_type == definition::StorageType::Float)
    {
        return format_like_qt_g(static_cast<double>(std::bit_cast<float>(static_cast<std::int32_t>(raw))), 6);
    }
    if (definition::is_unsigned_storage(spec.storage_type))
    {
        return std::to_string(static_cast<std::uint32_t>(raw));
    }
    switch (definition::storage_byte_size(spec.storage_type))
    {
    case 1:
        return std::to_string(static_cast<int>(static_cast<std::int8_t>(raw)));
    case 2:
        return std::to_string(static_cast<int>(static_cast<std::int16_t>(raw)));
    case 4:
        return std::to_string(static_cast<std::int32_t>(raw));
    default:
        return {};
    }
}

} // namespace

std::uint64_t element_byte_address(const MapElementSpec& spec, std::uint32_t index, bool for_write)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);
    std::uint64_t address = spec.address + std::uint64_t(index) * width;

    // Legacy applies two DIFFERENT wrx02 relocation predicates on the read and
    // write paths. Preserved verbatim and kept visibly side by side; the spec's
    // defect (a) covers the divergence, deferred pending corpus evidence about
    // which predicate real wrx02 ROMs actually need -- deliberately NOT
    // reconciled by the 6b-4 fix wave (unlike the byte-order defects it does
    // fix), since picking the wrong one here could target a different but
    // still-plausible ROM address on a real device.
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
    // correctly endian-aware unsigned assembly used for uint reads below AND
    // (as of the 6b-4 fix) for signed reads too, and `byte_value`, filled
    // most-significant-byte-first in BOTH endian branches -- still needed
    // below for the float branch's bit-pattern assembly, which is
    // legitimately endian-independent (floats are always big-endian-in-ROM;
    // see that branch's comment). `byte_value` is no longer used to
    // reconstruct signed integers: that reconstruction used to reinterpret
    // it through a union-member layout that assumed a fill order different
    // from how it was actually filled here, byte-swapping every signed
    // multi-byte read -- fixed by reading `data_byte` directly instead (see
    // the signed-integer branch below).
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
        // Assembles the four bytes into a uint32_t and returns those bits
        // reinterpreted as int32_t, rather than reading a union member that
        // was never written (UB, what legacy did). Going uint32_t ->
        // std::bit_cast<float> -> std::bit_cast<std::int32_t> and back would
        // round-trip to the exact same bit pattern (bit_cast never alters
        // the underlying bits, for any input including NaN patterns), so
        // that detour is skipped in favor of a single direct bit_cast; the
        // "reinterpret these bytes as the float's bit pattern" intent is
        // unchanged, only the intermediate `float` local is gone.
        //
        // Legacy reads map_data_value.float_value out of a union whose
        // float member overlaps the same byte_value[4] used above. On a
        // little-endian host that union member's least-significant byte is
        // byte_value[0]. Since byte_value was itself filled from
        // address+width-1 down to address+0 (see the little_or_float branch
        // above, always taken for float storage), this makes address+0 the
        // float's most-significant byte: a big-endian float in ROM, matching
        // decode_scaled_values's documented float handling in
        // calibration_service.cpp.
        const std::uint32_t bits = std::uint32_t(byte_value[0]) | (std::uint32_t(byte_value[1]) << 8) |
                                   (std::uint32_t(byte_value[2]) << 16) | (std::uint32_t(byte_value[3]) << 24);
        return static_cast<std::int64_t>(std::bit_cast<std::int32_t>(bits));
    }

    // Signed integer storage, every width including 24-bit. `data_byte` is
    // already correctly endian-assembled above (used unconditionally for
    // unsigned reads too) -- sign-extending it directly is both the fix for
    // the byte-swap defect that used to live here (a MSB-first
    // reconstruction through byte_value[], mismatched against how
    // byte_value[] was actually filled for the "little" branch) and for the
    // int24-always-zero defect (the old code never handled width == 3 at
    // all). Matches decode_scaled_values's identical
    // little_endian-read-then-sign_extend(raw, width) pattern
    // (calibration_service.cpp) for every width.
    return static_cast<std::int64_t>(sign_extend(data_byte, width));
}

Result<std::vector<std::uint8_t>> write_raw_element(const MapElementSpec& spec, std::int64_t raw)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);
    const bool is_float = spec.storage_type == definition::StorageType::Float;

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
                                                      int float_precision)
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

    return write_raw_element(spec, raw);
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

Result<EditPatch> apply_increment(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t x_size,
                                  std::span<const std::string_view> cell_text, const SelectionRange& range,
                                  IncrementStep step, int float_precision)
{
    // Change 1 (see this function's doc comment): the zero-increment check
    // moves before the loop and fails the whole call, rather than raising a
    // modal inside a retry loop a zero increment could never exit.
    if (spec.coarse_increment == 0.0 || spec.fine_increment == 0.0)
    {
        return fail(ErrorKind::InvalidConfig, "fine or coarse increment is zero or unset in the definition");
    }

    const std::string map_value_storagetype = definition::storage_type_text(spec.storage_type);
    const float map_coarse_inc_value = static_cast<float>(spec.coarse_increment);
    const float map_fine_inc_value = static_cast<float>(spec.fine_increment);

    // Computes the to_byte-encoded, rounded text for one candidate display
    // value, reproducing the (identical) computation legacy repeats in all
    // three do-while branches: `QString::number(expression_evaluate(to_byte,
    // QString::number(value), float_precision))`, then, for every non-float
    // storage type, `QString::number(qRound(that.toFloat()))`. Both
    // QString::number calls here are bare -- precision 6, not float_precision
    // (see this function's doc comment) -- and qRound returns a 32-bit int,
    // reproduced with llround + a narrowing cast to std::int32_t rather than
    // std::llround's own 64-bit result.
    const auto compute_to_byte_text = [&](float value)
    {
        const double encoded =
            expression_evaluate(spec.to_byte, format_like_qt_g(static_cast<double>(value), 6), float_precision);
        std::string text = format_like_qt_g(encoded, 6);
        if (map_value_storagetype != "float")
        {
            const auto rounded = static_cast<std::int32_t>(std::llround(static_cast<double>(to_float_or_zero(text))));
            text = std::to_string(rounded);
        }
        return text;
    };

    // The storage-type saturation and sign-wrap guards (:347-400 in the
    // legacy source), preserved exactly as legacy has them -- string
    // comparisons against "uint8"/"int16"/etc., not enum comparisons; two
    // independent top-level checks (uint* then int*), not an if/else-if,
    // matching legacy's two separate `if` blocks. `rom_data_value` is the
    // pre-increment cell's raw value, formatted the same way `candidate` is;
    // legacy's `break` (which exits the whole do-while the instant a guard
    // fires, skipping every later check) becomes an early `return` here,
    // preserving the same short-circuit. This inconsistency with set_value's
    // and paste_value's guards is the spec's defect (c), fixed in a later
    // task, not here.
    const auto apply_saturation_guard = [&](std::string_view rom_data_value, std::string candidate)
    {
        if (map_value_storagetype.starts_with("uint"))
        {
            if (map_value_storagetype == "uint8" && to_uint32_or_zero(candidate) > 0xFFU)
            {
                return std::string(rom_data_value);
            }
            if (map_value_storagetype == "uint16" && to_uint32_or_zero(candidate) > 0xFFFFU)
            {
                return std::string(rom_data_value);
            }
            if (map_value_storagetype == "uint32" && to_uint32_or_zero(candidate) > 0xFFFFFFFFU)
            {
                return std::string(rom_data_value);
            }
            if (to_int32_or_zero(candidate) < 0)
            {
                return std::string(rom_data_value);
            }
        }
        if (map_value_storagetype.starts_with("int"))
        {
            const std::uint32_t rom_u32 = to_uint32_or_zero(rom_data_value);
            const std::int32_t rom_i32 = to_int32_or_zero(rom_data_value);
            const std::uint32_t cand_u32 = to_uint32_or_zero(candidate);
            const std::int32_t cand_i32 = to_int32_or_zero(candidate);

            if (map_value_storagetype == "int8" && ((rom_u32 <= 0x7FU && cand_u32 > 0x7FU) ||
                                                    (static_cast<std::uint8_t>(rom_i32) >= 0x80U &&
                                                     static_cast<std::uint8_t>(cand_i32) < 0x80U && cand_i32 != 0)))
            {
                return std::string(rom_data_value);
            }
            if (map_value_storagetype == "int16" && ((rom_u32 <= 0x7FFFU && cand_u32 > 0x7FFFU) ||
                                                     (static_cast<std::uint16_t>(rom_i32) >= 0x8000U &&
                                                      static_cast<std::uint16_t>(cand_i32) < 0x8000U && cand_i32 != 0)))
            {
                return std::string(rom_data_value);
            }
            if ((map_value_storagetype == "int32" || map_value_storagetype == "float") &&
                ((rom_u32 <= 0x7FFFFFFFU && cand_u32 > 0x7FFFFFFFU) ||
                 (static_cast<std::uint32_t>(rom_i32) >= 0x80000000U &&
                  static_cast<std::uint32_t>(cand_i32) < 0x80000000U && cand_i32 != 0)))
            {
                return std::string(rom_data_value);
            }
        }
        return candidate;
    };

    EditPatch patch;
    for (int j = range.first_row; j <= range.last_row; ++j)
    {
        for (int i = range.first_col; i <= range.last_col; ++i)
        {
            const auto index = static_cast<std::uint32_t>(j) * x_size + static_cast<std::uint32_t>(i);

            const auto raw_before = read_raw_element(rom_data, spec, index);
            if (!raw_before.has_value())
            {
                return std::unexpected(raw_before.error());
            }
            const std::string rom_data_value = format_raw_value_display(spec, *raw_before);

            float map_item_value = to_float_or_zero(cell_text[index]);
            switch (step)
            {
            case IncrementStep::FineUp:
                map_item_value += map_fine_inc_value;
                break;
            case IncrementStep::FineDown:
                map_item_value -= map_fine_inc_value;
                break;
            case IncrementStep::CoarseUp:
                map_item_value += map_coarse_inc_value;
                break;
            case IncrementStep::CoarseDown:
                map_item_value -= map_coarse_inc_value;
                break;
            }

            std::string new_rom_data_value;
            if (spec.min_value != " " && map_item_value < to_float_or_zero(spec.min_value))
            {
                map_item_value = to_float_or_zero(spec.min_value);
                new_rom_data_value = compute_to_byte_text(map_item_value);
            }
            else if (spec.max_value != " " && map_item_value > to_float_or_zero(spec.max_value))
            {
                map_item_value = to_float_or_zero(spec.max_value);
                new_rom_data_value = compute_to_byte_text(map_item_value);
            }
            else
            {
                new_rom_data_value = apply_saturation_guard(rom_data_value, compute_to_byte_text(map_item_value));
            }

            // `expression_evaluate`'s "x" input here is new_rom_data_value
            // itself, not a bare QString::number(...) call -- float_precision
            // is the correct value for this expression_evaluate's precision
            // argument (see this function's doc comment).
            const double reversed = expression_evaluate(spec.from_byte, new_rom_data_value, float_precision);
            const std::string display_text = format_like_qt_g(reversed, 6);

            const auto encoded = write_raw_element(spec, raw_from_display_text(spec, new_rom_data_value));
            if (!encoded.has_value())
            {
                return std::unexpected(encoded.error());
            }

            patch.push_back({.index = index,
                             .display_text = display_text,
                             .byte_address = element_byte_address(spec, index, /*for_write=*/true),
                             .bytes = *encoded});
        }
    }

    return patch;
}

Result<EditPatch> apply_set_expression(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t x_size,
                                       std::span<const std::string_view> cell_text, const SelectionRange& range,
                                       std::string_view input, int float_precision)
{
    // rom_data is unused: set_value reads the pre-edit raw value
    // (read_rom_data_value) into a local that is unconditionally overwritten
    // below, before it is ever read -- porting that read would add a
    // ROM-bounds check with no effect on this function's output, so it's
    // dropped along with the other dead code (qDebug calls) this port
    // drops. Kept in the signature for parity with apply_increment's shape.
    (void)rom_data;

    const std::string map_value_storagetype = definition::storage_type_text(spec.storage_type);

    // Extracts the segment between the FIRST and SECOND occurrence of
    // `delimiter` in `text` -- QString::split(delimiter)[1]. Same technique
    // as map_value_decimal_count's '.'-segment parsing: for the common case
    // of no repeated delimiter (e.g. "+5"), this is just "everything after
    // the first occurrence," since there is no second occurrence.
    const auto operand_after = [](std::string_view text, char delimiter) -> std::string_view
    {
        const std::size_t first = text.find(delimiter);
        if (first == std::string_view::npos)
        {
            return {};
        }
        const std::size_t second = text.find(delimiter, first + 1);
        return text.substr(first + 1, second == std::string_view::npos ? std::string_view::npos : second - (first + 1));
    };

    const char op = input.empty() ? '\0' : input.front();
    float operand = 0.0F;
    if (op == '+' || op == '-' || op == '*' || op == '/')
    {
        operand = to_float_or_zero(operand_after(input, op));
    }
    // Change 1 (see this function's doc comment): the divide-by-zero check
    // moves before the loop and fails the whole call, rather than raising a
    // modal and silently continuing with the unmodified value. The divisor
    // is the same for every cell, so checking once up front is equivalent
    // to checking on the first iteration.
    if (op == '/' && operand == 0.0F)
    {
        return fail(ErrorKind::InvalidConfig, "cannot divide by zero");
    }
    const float absolute_value = to_float_or_zero(input);

    EditPatch patch;
    for (int j = range.first_row; j <= range.last_row; ++j)
    {
        for (int i = range.first_col; i <= range.last_col; ++i)
        {
            const auto index = static_cast<std::uint32_t>(j) * x_size + static_cast<std::uint32_t>(i);

            float map_item_value = to_float_or_zero(cell_text[index]);
            switch (op)
            {
            case '+':
                map_item_value += operand;
                break;
            case '-':
                map_item_value -= operand;
                break;
            case '*':
                map_item_value *= operand;
                break;
            case '/':
                map_item_value /= operand;
                break;
            default:
                map_item_value = absolute_value;
                break;
            }

            // Min/max clamp, ported verbatim from set_value -- no
            // storage-type saturation guard runs here, unlike
            // apply_increment (spec's defect (c); see this function's doc
            // comment).
            if (spec.min_value != " " && map_item_value < to_float_or_zero(spec.min_value))
            {
                map_item_value = to_float_or_zero(spec.min_value);
            }
            if (spec.max_value != " " && map_item_value > to_float_or_zero(spec.max_value))
            {
                map_item_value = to_float_or_zero(spec.max_value);
            }

            const double encoded = expression_evaluate(
                spec.to_byte, format_like_qt_g(static_cast<double>(map_item_value), 6), float_precision);
            std::string rom_data_value = format_like_qt_g(encoded, 6);
            if (map_value_storagetype != "float")
            {
                rom_data_value = std::to_string(
                    static_cast<std::int32_t>(std::llround(static_cast<double>(to_float_or_zero(rom_data_value)))));
            }

            // `expression_evaluate`'s "x" input here is rom_data_value
            // itself, not a bare QString::number(...) call -- float_precision
            // is the correct value for this expression_evaluate's precision
            // argument (see this function's doc comment).
            const double reversed = expression_evaluate(spec.from_byte, rom_data_value, float_precision);
            const std::string display_text = format_like_qt_g(reversed, 6);

            const auto encoded_bytes = write_raw_element(spec, raw_from_display_text(spec, rom_data_value));
            if (!encoded_bytes.has_value())
            {
                return std::unexpected(encoded_bytes.error());
            }

            patch.push_back({.index = index,
                             .display_text = display_text,
                             .byte_address = element_byte_address(spec, index, /*for_write=*/true),
                             .bytes = *encoded_bytes});
        }
    }

    return patch;
}

} // namespace fastecu::calibration
