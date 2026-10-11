#include "src/backend/calibration/map_edit.h"

#include <array>
#include <bit>
#include <format>
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
    // sentinel address a real ROM can never contain rather than wrapping, so
    // every caller's existing bounds check (read_raw_element's
    // byte_window_fits, or read_raw_element having already validated the
    // same index before any of this file's apply_* operations compute the
    // same address again) turns the overflow
    // into the same "runs past ROM size" failure decode_numeric_run reports,
    // instead of silently targeting a wrapped-around address.
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

Result<std::int64_t> ReadRawElement(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t index)
{
    const std::uint32_t width = definition::StorageByteSize(spec.storage_type);
    const std::uint64_t address = ElementByteAddress(spec, index);

    if (!ByteWindowFits(rom_data, address, width))
    {
        return Fail(ErrorKind::kInternal,
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

    const bool little_or_float = spec.endian == "little" || spec.storage_type == definition::StorageType::kFloat;

    for (std::uint32_t k = 0; k < width; ++k)
    {
        const std::uint64_t src = little_or_float ? address + width - 1 - k : address + k;
        const std::uint8_t raw_byte = rom_data[static_cast<std::size_t>(src)];
        data_byte = (data_byte << 8U) + raw_byte;
        byte_value[k] = raw_byte;
    }

    if (definition::IsUnsignedStorage(spec.storage_type))
    {
        return static_cast<std::int64_t>(data_byte);
    }

    if (spec.storage_type == definition::StorageType::kFloat)
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
        // decode_numeric_run's documented float handling in
        // calibration_service.cpp.
        const std::uint32_t bits = bytes::ReadU32Le(byte_value);
        return static_cast<std::int64_t>(std::bit_cast<std::int32_t>(bits));
    }

    // Signed integer storage, every width including 24-bit. `data_byte` is
    // already correctly endian-assembled above (used unconditionally for
    // unsigned reads too) -- sign-extending it directly is both the fix for
    // the byte-swap defect that used to live here (a MSB-first
    // reconstruction through byte_value[], mismatched against how
    // byte_value[] was actually filled for the "little" branch) and for the
    // int24-always-zero defect (the old code never handled width == 3 at
    // all). Matches decode_numeric_run's identical
    // little_endian-read-then-sign_extend(raw, width) pattern
    // (calibration_service.cpp) for every width.
    return static_cast<std::int64_t>(SignExtend(data_byte, width));
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
