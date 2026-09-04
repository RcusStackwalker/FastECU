#include "src/backend/calibration/map_edit.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <format>
#include <limits>
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

// Generalizes QString::toInt()'s whole-string-or-0 semantics (same rationale
// as to_uint32_or_zero above) over every signed width this file needs:
// int32_t for sign_wrap_heuristic_fires' checks below (matching Qt's qint32),
// and int64_t for raw_from_display_text's non-float branch.
template <std::signed_integral T> T to_signed_or_zero(std::string_view text)
{
    T value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return 0;
    }
    return value;
}

// The genuine range/overflow half of legacy's storage-type guard block
// (:347-400 in the legacy source): every sub-condition here answers "does
// this candidate actually fit the storage type", a question whose answer does
// not depend on the edit being an increment. Split out of
// apply_saturation_guard below in the PR 6b-4 final-review fix wave (finding
// C1) so encode_guarded -- whose callers treat a fired guard as a
// whole-operation hard failure -- can apply THIS check alone, without the
// sign-wrap heuristic that only has meaning inside apply_increment's
// revert-and-continue retry loop.
//
// Preserved exactly as legacy has it otherwise -- string comparisons against
// "uint8"/"int16"/etc., not enum comparisons; two independent top-level
// checks (uint* then int*), not an if/else-if, matching legacy's two separate
// `if` blocks. `rom_data_value` is the pre-edit cell's raw value, formatted
// the same way `candidate` is; the signed clauses compare the two as UNSIGNED
// parses, which is what makes them a range check: they fire only when the
// candidate's byte pattern would represent a value the storage width cannot
// hold and the pre-edit value did fit.
//
// Two legacy quirks are deliberately carried rather than fixed: `int24` and
// `uint24` have no width clause at all (a uint24 candidate is only rejected
// for being negative), and the `"float"` arm of the int32 clause is
// unreachable -- no `"float"` ever satisfies the enclosing
// `starts_with("int")`, so float-storage maps get no range check whatsoever.
// Both match legacy; neither is in scope to change here (see the spec's
// defect (c) section).
bool storage_range_check_fires(std::string_view storage_type, std::string_view rom_data_value,
                               std::string_view candidate)
{
    if (storage_type.starts_with("uint"))
    {
        if (storage_type == "uint8" && to_uint32_or_zero(candidate) > 0xFFU)
        {
            return true;
        }
        if (storage_type == "uint16" && to_uint32_or_zero(candidate) > 0xFFFFU)
        {
            return true;
        }
        if (storage_type == "uint32" && to_uint32_or_zero(candidate) > 0xFFFFFFFFU)
        {
            return true;
        }
        // A negative candidate never fits ANY unsigned storage type,
        // regardless of context -- a genuine range check, not a heuristic,
        // so it belongs on this side of the split.
        if (to_signed_or_zero<std::int32_t>(candidate) < 0)
        {
            return true;
        }
    }
    if (storage_type.starts_with("int"))
    {
        const std::uint32_t rom_u32 = to_uint32_or_zero(rom_data_value);
        const std::uint32_t cand_u32 = to_uint32_or_zero(candidate);

        if (storage_type == "int8" && rom_u32 <= 0x7FU && cand_u32 > 0x7FU)
        {
            return true;
        }
        if (storage_type == "int16" && rom_u32 <= 0x7FFFU && cand_u32 > 0x7FFFU)
        {
            return true;
        }
        if ((storage_type == "int32" || storage_type == "float") && rom_u32 <= 0x7FFFFFFFU && cand_u32 > 0x7FFFFFFFU)
        {
            return true;
        }
    }
    return false;
}

// The OTHER half of legacy's guard block, and deliberately NOT a range check:
// it fires whenever a signed cell's byte pattern had its high bit set before
// the edit and does not after (a negative-to-positive transition), whether or
// not both values are perfectly representable in the storage type. `int8`
// -1 -> 3 fires it; so does every ordinary edit that moves a signed cell from
// negative to positive, which is a routine operation on any signed-storage
// calibration map (timing trim, fuel trim, MAF correction).
//
// That makes this a HEURISTIC, meaningful in exactly one place: inside
// apply_increment's bounded retry loop, as legacy's crude proxy for "this
// increment wrapped around from positive overflow into a negative bit
// pattern". There, firing costs one cell -- it reverts to its pre-edit value
// and the loop moves on -- so a false positive is bounded and legacy-faithful.
// Applied anywhere without that revert-and-continue structure it is simply
// wrong: encode_guarded's callers fail the WHOLE selection's edit on a fired
// guard, which would reject ordinary valid signed edits outright. That is
// finding C1 of the PR 6b-4 final review, and is why this predicate is
// reachable only through apply_saturation_guard (apply_increment's guard) and
// never through encode_guarded.
bool sign_wrap_heuristic_fires(std::string_view storage_type, std::string_view rom_data_value,
                               std::string_view candidate)
{
    if (!storage_type.starts_with("int"))
    {
        return false;
    }

    const std::int32_t rom_i32 = to_signed_or_zero<std::int32_t>(rom_data_value);
    const std::int32_t cand_i32 = to_signed_or_zero<std::int32_t>(candidate);

    if (storage_type == "int8" && static_cast<std::uint8_t>(rom_i32) >= 0x80U &&
        static_cast<std::uint8_t>(cand_i32) < 0x80U && cand_i32 != 0)
    {
        return true;
    }
    if (storage_type == "int16" && static_cast<std::uint16_t>(rom_i32) >= 0x8000U &&
        static_cast<std::uint16_t>(cand_i32) < 0x8000U && cand_i32 != 0)
    {
        return true;
    }
    // Same dead `"float"` arm as storage_range_check_fires' int32 clause, and
    // preserved for the same reason: it is what legacy wrote.
    if ((storage_type == "int32" || storage_type == "float") && static_cast<std::uint32_t>(rom_i32) >= 0x80000000U &&
        static_cast<std::uint32_t>(cand_i32) < 0x80000000U && cand_i32 != 0)
    {
        return true;
    }
    return false;
}

// apply_increment's guard: legacy's full guard block, both halves ORed
// together exactly as legacy has them. Lifted to file scope in the 6b-4 fix
// wave (spec's defect (c)) so apply_increment does not carry a private copy
// of the range check encode_guarded also needs.
//
// Returns nullopt when either half fires -- distinct from returning a
// candidate that merely equals rom_data_value because the requested value
// hasn't actually moved the quantized result. apply_increment's bounded retry
// loop depends on that distinction (see its own doc comment in map_edit.h for
// why, and how it's preserved).
//
// encode_guarded deliberately does NOT call this function: it calls
// storage_range_check_fires alone. A guard firing there is an unconditional
// hard failure of the whole edit, and only the range half of this check
// carries a meaning that survives being applied that way -- see
// sign_wrap_heuristic_fires' own comment for the full argument.
std::optional<std::string> apply_saturation_guard(std::string_view storage_type, std::string_view rom_data_value,
                                                  std::string candidate)
{
    if (storage_range_check_fires(storage_type, rom_data_value, candidate) ||
        sign_wrap_heuristic_fires(storage_type, rom_data_value, candidate))
    {
        return std::nullopt;
    }
    return candidate;
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
        // numeric value (to_float_or_zero's whole-string-or-0.0F parse is
        // exactly what's needed here), then bit_cast to get the IEEE-754 bit
        // pattern, NOT parsed as an integer.
        return static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(to_float_or_zero(text)));
    }
    // Every other storage type: parse as a plain integer. Mirrors
    // QString::toInt()'s whole-string-must-parse-or-0 semantics -- the
    // inputs this function actually receives are always machine-generated
    // integer strings, so to_signed_or_zero's fallback is sufficient.
    return to_signed_or_zero<std::int64_t>(text);
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

// The inverse of write_raw_element's packing loop -- unpacks a small,
// already-encoded byte buffer (encode_guarded's own return value, not
// ROM-addressed data) back into the raw representation format_raw_value_display
// expects. Used by display_text_after_encode below so a caller's displayed
// text is always derived from the SAME bytes actually written, rather than
// from a separately re-derived candidate that could in principle drift from
// them (a different formatting precision, a different rounding path).
std::int64_t unpack_raw(const MapElementSpec& spec, std::span<const std::uint8_t> encoded)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);
    const bool is_float = spec.storage_type == definition::StorageType::Float;
    const bool little_endian = !is_float && (spec.endian == "little");

    std::uint32_t packed = 0;
    for (std::uint32_t k = 0; k < width; ++k)
    {
        const std::uint32_t shift = little_endian ? (8U * k) : (8U * (width - 1U - k));
        packed |= (static_cast<std::uint32_t>(encoded[k]) << shift);
    }

    if (definition::is_unsigned_storage(spec.storage_type))
    {
        return static_cast<std::int64_t>(packed);
    }
    if (is_float)
    {
        return static_cast<std::int64_t>(std::bit_cast<std::int32_t>(packed));
    }
    return static_cast<std::int64_t>(sign_extend(packed, width));
}

// Derives a CellPatch's display_text from the bytes an encode call (e.g.
// encode_guarded) actually returned, rather than from a value computed
// separately alongside them -- decode the written bytes, format them the way
// every read path in this file does, then run that text back through
// spec.from_byte, exactly mirroring apply_increment's own final display-text
// step (map_edit.cpp's apply_increment). Guarantees display_text and .bytes
// can never disagree, which matters once a value may be clamped or
// guard-narrowed away from what the caller originally asked for.
std::string display_text_after_encode(const MapElementSpec& spec, std::span<const std::uint8_t> encoded,
                                      int float_precision)
{
    const std::string rom_data_value = format_raw_value_display(spec, unpack_raw(spec, encoded));
    const double reversed = expression_evaluate(spec.from_byte, rom_data_value, float_precision);
    return format_like_qt_g(reversed, 6);
}

} // namespace

std::uint64_t element_byte_address(const MapElementSpec& spec, std::uint32_t index, bool for_write)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);

    // Layout matches decode_scaled_values (calibration_service.cpp) exactly --
    // spec's defect (b): the edit path used to lay elements out flat
    // (address + index*width), ignoring start_position/interval, which
    // decode_scaled_values honours. addr(j) = address + (start_position-1) *
    // width + j * width * interval.
    //
    // start_position is 1-based. definition_resolver rejects 0, but clamp
    // identically to decode_scaled_values' guard so the two paths cannot
    // disagree if this is ever reached with an out-of-domain 0 directly.
    const std::uint64_t start_offset = spec.start_position == 0 ? 0 : std::uint64_t(spec.start_position - 1);

    // Overflow-checked exactly as decode_scaled_values: on overflow, return a
    // sentinel address a real ROM can never contain rather than wrapping, so
    // every caller's existing bounds check (read_raw_element's
    // byte_window_fits, or read_raw_element having already validated the
    // same index before any of this file's apply_* operations reach their
    // own element_byte_address(..., for_write=true) call) turns the overflow
    // into the same "runs past ROM size" failure decode_scaled_values reports,
    // instead of silently targeting a wrapped-around address.
    constexpr std::uint64_t kOverflowSentinel = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t start_byte_offset = 0;
    std::uint64_t stride = 0;
    std::uint64_t element_offset = 0;
    std::uint64_t address = 0;
    if (!checked_multiply(start_offset, width, start_byte_offset) || !checked_multiply(width, spec.interval, stride) ||
        !checked_multiply(std::uint64_t(index), stride, element_offset) ||
        !checked_add(spec.address, start_byte_offset, address) || !checked_add(address, element_offset, address))
    {
        return kOverflowSentinel;
    }

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
        data_byte = (data_byte << 8U) + raw_byte;
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
        const std::uint32_t bits = bytes::readU32Le(byte_value);
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

Result<std::vector<std::uint8_t>> encode_guarded(bytes::ByteView rom_data, const MapElementSpec& spec,
                                                 std::uint32_t index, double display_value, int format_precision,
                                                 int float_precision)
{
    // Clamp to the definition's min/max first -- the " " = unset convention,
    // same as every other clamp in this file.
    double clamped = display_value;
    if (spec.min_value != " " && clamped < to_float_or_zero(spec.min_value))
    {
        clamped = to_float_or_zero(spec.min_value);
    }
    if (spec.max_value != " " && clamped > to_float_or_zero(spec.max_value))
    {
        clamped = to_float_or_zero(spec.max_value);
    }

    const auto raw_before = read_raw_element(rom_data, spec, index);
    if (!raw_before.has_value())
    {
        return std::unexpected(raw_before.error());
    }
    const std::string rom_data_value = format_raw_value_display(spec, *raw_before);
    const std::string map_value_storagetype = definition::storage_type_text(spec.storage_type);
    const bool is_float = spec.storage_type == definition::StorageType::Float;

    // `format_precision` formats `clamped` into the "x" input text for
    // spec.to_byte -- NOT the same thing as `float_precision`, which is only
    // expression_evaluate's third argument (intermediate-rounding precision
    // for multi-operator expressions). Callers with a legacy-fidelity
    // contract (apply_set_expression, apply_interpolation: bare
    // QString::number, precision 6, regardless of float_precision -- see
    // their own doc comments in map_edit.h) pass 6 here; a caller with no
    // such contract may pass float_precision instead. This split exists
    // specifically so this function can NOT be built by delegating to
    // encode_scaled_value (whose own `float_precision` parameter drives both
    // the formatting AND the expression_evaluate precision at once, by
    // design, for its own different caller contract -- see its doc comment).
    //
    // The guard needs the WIDE (pre-truncation) candidate, exactly as
    // apply_increment's own compute_to_byte_text produces -- the packed
    // bytes below are already narrowed to the storage width, which would
    // silently discard the very overflow the guard exists to catch (e.g. 300
    // truncates to 0x2C in a uint8 well before it ever reaches
    // write_raw_element). Computed once here and reused for both the guard
    // check and the actual write below, so the two can never disagree.
    const double to_byte_encoded =
        expression_evaluate(spec.to_byte, format_like_qt_g(clamped, format_precision), float_precision);
    std::string candidate = format_like_qt_g(to_byte_encoded, format_precision);
    if (!is_float)
    {
        candidate = std::to_string(static_cast<std::int32_t>(std::llround(to_byte_encoded)));
    }

    // Only the GENUINE range/overflow half of the guard, never the sign-wrap
    // heuristic apply_increment also applies (finding C1 of the PR 6b-4 final
    // review; see sign_wrap_heuristic_fires' comment above). A guard firing
    // here fails the whole selection's edit, and "you changed a negative cell
    // to a positive one" -- which is all the heuristic detects when both
    // values are in range -- is an ordinary, valid edit on any signed-storage
    // map, not an overflow.
    if (storage_range_check_fires(map_value_storagetype, rom_data_value, candidate))
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("value would overflow storage type {}", map_value_storagetype));
    }

    // Packs the SAME to_byte_encoded value the guard just checked -- matches
    // encode_scaled_value's own round/bit_cast formulas exactly (see that
    // function), just without re-running expression_evaluate a second time
    // with a possibly-different formatting precision.
    std::int64_t raw = 0;
    if (is_float)
    {
        raw = static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(static_cast<float>(to_byte_encoded)));
    }
    else
    {
        raw = static_cast<std::int64_t>(static_cast<std::uint32_t>(std::llround(to_byte_encoded)));
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

    // The storage-type saturation and sign-wrap guard is apply_saturation_guard
    // (file scope, above), whose range-check half encode_guarded also uses as
    // of the 6b-4 fix wave (spec's defect (c)). This function alone applies
    // BOTH halves -- the range check and the sign-wrap heuristic -- because
    // this is the one context where a false positive is bounded: the guard
    // reverts just this cell and the retry loop continues with the next one.
    // That control flow is untouched, and is why this function calls the guard
    // directly rather than going through encode_guarded (see apply_increment's
    // doc comment in map_edit.h, and sign_wrap_heuristic_fires' comment above).
    //
    // Returns nullopt when a storage-type saturation/sign-wrap guard fires
    // (legacy's unconditional `break`, reverting to rom_data_value and
    // stopping the retry outright) -- distinct from returning a candidate
    // that merely equals rom_data_value because the increment hasn't moved
    // the quantized value yet (which the caller's retry loop must keep going
    // on). Both cases would look identical as a bare string return, which is
    // exactly the ambiguity a bounded-retry loop cannot tolerate: a
    // guard-triggered revert is terminal even when its resulting text
    // happens to equal rom_data_value; a genuinely-unchanged candidate must
    // retry.

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

            // Bounded retry (see this function's doc comment): keeps adding
            // the increment until the ENCODED (to_byte-rounded) value
            // actually changes, matching legacy's `do { ... } while
            // (rom_data_value == new_rom_data_value)` -- this matters
            // whenever to_byte maps several display values onto one stored
            // raw value (any scaling coarser than 1:1). `map_item_value` is
            // declared outside the loop and accumulates on every retry
            // iteration, exactly as legacy's does. Unlike legacy, the retry
            // is bounded at kMaxIncrementAttempts to guarantee termination
            // for a pathological to_byte that never responds to its input
            // (e.g. a constant expression) -- legacy's version was genuinely
            // unbounded there. The min/max clamp branches stay terminal,
            // matching legacy's unconditional `break` after clamping,
            // regardless of whether the resulting encoded value differs from
            // the original.
            constexpr int kMaxIncrementAttempts = 1000;

            float map_item_value = to_float_or_zero(cell_text[index]);
            std::string new_rom_data_value;
            bool resolved = false;

            for (int attempt = 0; attempt < kMaxIncrementAttempts && !resolved; ++attempt)
            {
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

                if (spec.min_value != " " && map_item_value < to_float_or_zero(spec.min_value))
                {
                    map_item_value = to_float_or_zero(spec.min_value);
                    new_rom_data_value = compute_to_byte_text(map_item_value);
                    resolved = true;
                    continue;
                }
                if (spec.max_value != " " && map_item_value > to_float_or_zero(spec.max_value))
                {
                    map_item_value = to_float_or_zero(spec.max_value);
                    new_rom_data_value = compute_to_byte_text(map_item_value);
                    resolved = true;
                    continue;
                }

                const auto guarded =
                    apply_saturation_guard(map_value_storagetype, rom_data_value, compute_to_byte_text(map_item_value));
                if (!guarded.has_value())
                {
                    // A guard fired -- terminal, even though the value
                    // didn't move (see apply_saturation_guard's doc
                    // comment).
                    new_rom_data_value = rom_data_value;
                    resolved = true;
                    continue;
                }
                new_rom_data_value = *guarded;
                if (new_rom_data_value != rom_data_value)
                {
                    resolved = true; // genuinely changed -- done
                }
                // else: unchanged and no guard fired -- loop again, bounded
                // by attempt < kMaxIncrementAttempts.
            }

            if (!resolved)
            {
                return fail(
                    ErrorKind::InvalidConfig,
                    std::format("increment did not change the stored value within {} attempts", kMaxIncrementAttempts));
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

            // Clamp, encode, and guard through the shared path (spec's
            // defect (c), fixed in 6b-4) -- see this function's doc comment.
            // format_precision is hardcoded to 6 here, NOT float_precision --
            // the legacy-fidelity contract this function's own doc comment
            // describes (bare QString::number, precision 6, regardless of
            // float_precision) still applies to the "x" input text; only the
            // clamp/guard logic is newly shared.
            const auto encoded_bytes = encode_guarded(rom_data, spec, index, static_cast<double>(map_item_value),
                                                      /*format_precision=*/6, float_precision);
            if (!encoded_bytes.has_value())
            {
                return std::unexpected(encoded_bytes.error());
            }

            const std::string display_text = display_text_after_encode(spec, *encoded_bytes, float_precision);

            patch.push_back({.index = index,
                             .display_text = display_text,
                             .byte_address = element_byte_address(spec, index, /*for_write=*/true),
                             .bytes = *encoded_bytes});
        }
    }

    return patch;
}

Result<EditPatch> apply_interpolation(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t x_size,
                                      std::span<const std::string_view> cell_text, const SelectionRange& range,
                                      InterpolationMode mode, int float_precision)
{
    // interpolate_value's interpolated values all come from cell_text -- but
    // rom_data IS read, once per cell, by the shared encode_guarded path
    // below (to fetch the guard's "previous value" comparison point).
    //
    // The selection's OWN local width/height -- sizes the transient
    // interpolation buffer below. Deliberately distinct from `x_size` (the
    // full run's width), which is used only to index into `cell_text`.
    const auto col_count = static_cast<std::uint32_t>(range.last_col - range.first_col + 1);
    const auto row_count = static_cast<std::uint32_t>(range.last_row - range.first_row + 1);

    // Replaces legacy's fixed `float cellValue[128][128]` (spec's defect
    // (e): indexed with the raw selection extent, so a selection wider or
    // taller than 128 wrote past the array) with a buffer sized exactly to
    // this selection. `at(col, row)` preserves legacy's `cellValue[i][j]` ==
    // `[col][row]` index order -- NOT `[row][col]` -- at every use below.
    std::vector<double> cell_values(std::size_t(col_count) * row_count, 0.0);
    const auto at = [col_count](std::uint32_t col, std::uint32_t row) { return std::size_t(row) * col_count + col; };

    // Reads one cell's current display text out of the full-run `cell_text`,
    // indexed by `x_size` (never col_count/row_count) -- mirrors
    // `map_data_cell_text.at(row * map_x_size + col).toFloat()`.
    const auto cell_at = [&](int row, int col)
    {
        return static_cast<double>(
            to_float_or_zero(cell_text[static_cast<std::size_t>(row) * x_size + static_cast<std::size_t>(col)]));
    };

    const double top_left = cell_at(range.first_row, range.first_col);
    const double top_right = cell_at(range.first_row, range.last_col);
    const double bottom_left = cell_at(range.last_row, range.first_col);
    const double bottom_right = cell_at(range.last_row, range.last_col);

    double left_row_adder = 0.0;
    double right_row_adder = 0.0;
    if (row_count > 1)
    {
        left_row_adder = (bottom_left - top_left) / static_cast<double>(row_count - 1);
        right_row_adder = (bottom_right - top_right) / static_cast<double>(row_count - 1);
    }

    for (std::uint32_t j = 0; j < row_count; ++j)
    {
        for (std::uint32_t i = 0; i < col_count; ++i)
        {
            cell_values[at(i, j)] =
                cell_at(range.first_row + static_cast<int>(j), range.first_col + static_cast<int>(i));
        }
    }

    switch (mode)
    {
    case InterpolationMode::Horizontal:
        for (std::uint32_t j = 0; j < row_count; ++j)
        {
            double col_adder = 0.0;
            if (col_count > 1)
            {
                col_adder =
                    (cell_values[at(col_count - 1, j)] - cell_values[at(0, j)]) / static_cast<double>(col_count - 1);
            }
            for (std::uint32_t i = 0; i < col_count; ++i)
            {
                if (col_count > 1)
                {
                    cell_values[at(i, j)] = cell_values[at(0, j)] + static_cast<double>(i) * col_adder;
                }
            }
        }
        break;
    case InterpolationMode::Vertical:
        for (std::uint32_t i = 0; i < col_count; ++i)
        {
            double row_adder = 0.0;
            if (row_count > 1)
            {
                row_adder =
                    (cell_values[at(i, row_count - 1)] - cell_values[at(i, 0)]) / static_cast<double>(row_count - 1);
            }
            for (std::uint32_t j = 0; j < row_count; ++j)
            {
                if (row_count > 1)
                {
                    cell_values[at(i, j)] = cell_values[at(i, 0)] + static_cast<double>(j) * row_adder;
                }
            }
        }
        break;
    case InterpolationMode::Bidirectional:
        // Pass 1: interpolate the left and right edges down the rows from
        // the four corners.
        for (std::uint32_t j = 0; j < row_count; ++j)
        {
            cell_values[at(0, j)] = cell_values[at(0, 0)] + static_cast<double>(j) * left_row_adder;
            if (col_count > 1)
            {
                cell_values[at(col_count - 1, j)] =
                    cell_values[at(col_count - 1, 0)] + static_cast<double>(j) * right_row_adder;
            }
        }
        // Pass 2: interpolate each row across between its (now-filled) edges
        // -- identical shape to the Horizontal branch above.
        for (std::uint32_t j = 0; j < row_count; ++j)
        {
            double col_adder = 0.0;
            if (col_count > 1)
            {
                col_adder =
                    (cell_values[at(col_count - 1, j)] - cell_values[at(0, j)]) / static_cast<double>(col_count - 1);
            }
            for (std::uint32_t i = 0; i < col_count; ++i)
            {
                if (col_count > 1)
                {
                    cell_values[at(i, j)] = cell_values[at(0, j)] + static_cast<double>(i) * col_adder;
                }
            }
        }
        break;
    }

    EditPatch patch;
    for (std::uint32_t j = 0; j < row_count; ++j)
    {
        for (std::uint32_t i = 0; i < col_count; ++i)
        {
            const auto index = static_cast<std::uint32_t>(range.first_row + static_cast<int>(j)) * x_size +
                               static_cast<std::uint32_t>(range.first_col + static_cast<int>(i));

            // Clamp, encode, and guard through the shared path (spec's
            // defect (c), fixed in 6b-4) -- see this function's doc comment.
            // format_precision is hardcoded to 6 here, NOT float_precision --
            // the legacy-fidelity contract this function's own doc comment
            // describes (bare QString::number, precision 6, regardless of
            // float_precision) still applies to the "x" input text; only the
            // clamp/guard logic is newly shared.
            const auto encoded_bytes = encode_guarded(rom_data, spec, index, cell_values[at(i, j)],
                                                      /*format_precision=*/6, float_precision);
            if (!encoded_bytes.has_value())
            {
                return std::unexpected(encoded_bytes.error());
            }

            const std::string display_text = display_text_after_encode(spec, *encoded_bytes, float_precision);

            patch.push_back({.index = index,
                             .display_text = display_text,
                             .byte_address = element_byte_address(spec, index, /*for_write=*/true),
                             .bytes = *encoded_bytes});
        }
    }

    return patch;
}

Result<EditPatch> apply_paste(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t x_size,
                              std::uint32_t y_size, std::span<const std::string_view> cell_text,
                              const SelectionRange& range, std::span<const std::vector<std::string_view>> pasted_rows,
                              int float_precision)
{
    // cell_text is unused: legacy's mapDataCellText.replace(index,
    // columns[i]) followed immediately by .at() on that SAME index always
    // reads back the pasted text itself, never a prior value from cell_text
    // -- see this function's doc comment. rom_data IS now read, once per
    // cell, by the shared encode_guarded path below (to fetch the guard's
    // "previous value" comparison point).
    (void)cell_text;

    const auto x_size_i = static_cast<int>(x_size);
    const auto y_size_i = static_cast<int>(y_size);

    // Legacy computes its column count once from the FIRST row's tab count
    // (`rows.first().count('\t') + 1`) and indexes every row unconditionally
    // at that width -- an out-of-bounds QStringList::operator[] (UB) for a
    // later, shorter ("ragged") row. Reproduced here as
    // pasted_rows.front().size() when non-empty, but each cell below is
    // additionally guarded against a short row instead of indexed
    // unconditionally -- see this function's doc comment.
    const std::size_t num_columns = pasted_rows.empty() ? 0 : pasted_rows.front().size();

    EditPatch patch;
    for (std::size_t row = 0; row < pasted_rows.size(); ++row)
    {
        const auto& columns = pasted_rows[row];
        for (std::size_t col = 0; col < num_columns; ++col)
        {
            // Ragged-row guard (see this function's doc comment): legacy has
            // no equivalent check and indexes unconditionally.
            if (col >= columns.size())
            {
                continue;
            }

            const int dest_row = range.first_row + static_cast<int>(row);
            const int dest_col = range.first_col + static_cast<int>(col);
            if (dest_row >= y_size_i || dest_col >= x_size_i)
            {
                continue;
            }

            const auto index = static_cast<std::uint32_t>(dest_row) * x_size + static_cast<std::uint32_t>(dest_col);
            const std::string_view text = columns[col];

            // Parsed to a number so it can be clamped and guarded -- see
            // this function's doc comment on why the "x" input to
            // expression_evaluate is no longer the pasted text verbatim as
            // of the 6b-4 fix (spec's defect (c)). Unlike apply_set_expression
            // and apply_interpolation, paste never had a precision-6
            // legacy-fidelity contract to preserve (legacy never formatted
            // its "x" input at all), so float_precision is used here for
            // format_precision too -- there is no established convention this
            // reformatting needs to match.
            const auto parsed_value = static_cast<double>(to_float_or_zero(text));
            const auto encoded_bytes = encode_guarded(rom_data, spec, index, parsed_value,
                                                      /*format_precision=*/float_precision, float_precision);
            if (!encoded_bytes.has_value())
            {
                return std::unexpected(encoded_bytes.error());
            }

            // display_text is now derived from the bytes actually written,
            // like every other apply_* operation -- see this function's doc
            // comment.
            const std::string display_text = display_text_after_encode(spec, *encoded_bytes, float_precision);
            patch.push_back({.index = index,
                             .display_text = display_text,
                             .byte_address = element_byte_address(spec, index, /*for_write=*/true),
                             .bytes = *encoded_bytes});
        }
    }

    return patch;
}

} // namespace fastecu::calibration
