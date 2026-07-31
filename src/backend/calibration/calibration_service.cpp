#include "src/backend/calibration/calibration_service.h"

#include <cstdio>
#include <cstring>

#include "src/algorithms/expression/expression_evaluator.h"

namespace fastecu::calibration
{

Status save_rom(std::span<const std::uint8_t> rom_data, std::string_view file_handle,
                IFileRepository& file_repository)
{
    return file_repository.write(file_handle, rom_data);
}

Result<std::vector<std::uint8_t>> open_rom(std::string_view file_handle,
                                           std::span<const std::uint8_t> preloaded_bytes,
                                           std::string_view backup_handle,
                                           IFileRepository& file_repository)
{
    if (!preloaded_bytes.empty())
    {
        // Fire-and-forget, matching open_subaru_rom_file's own behavior of
        // never checking this write's result.
        (void)save_rom(preloaded_bytes, backup_handle, file_repository);
        return std::vector<std::uint8_t>(preloaded_bytes.begin(), preloaded_bytes.end());
    }
    return file_repository.read(file_handle);
}

Status validate_rom_size(const definition::RomDefinition& rom_definition,
                         std::size_t rom_byte_length)
{
    for (const definition::CalibrationMap& map : rom_definition.maps)
    {
        if (map.address.has_value() && *map.address > rom_byte_length)
        {
            return fail(ErrorKind::InvalidConfig, "map address exceeds ROM size");
        }
        if (map.x_axis.address.has_value() && *map.x_axis.address > rom_byte_length)
        {
            return fail(ErrorKind::InvalidConfig, "x-axis address exceeds ROM size");
        }
        if (map.y_axis.address.has_value() && *map.y_axis.address > rom_byte_length)
        {
            return fail(ErrorKind::InvalidConfig, "y-axis address exceeds ROM size");
        }
    }
    return {};
}

std::vector<std::uint8_t> apply_flash_method_padding(
    std::vector<std::uint8_t> rom_data, std::string_view flash_method)
{
    constexpr std::size_t kInsertionPoint = 0x20000;
    constexpr std::size_t kPaddingBytes = 0x8000;
    constexpr std::size_t kSizeThreshold = 190 * 1024;

    if (!flash_method.starts_with("sub_ecu_denso_mc68hc16y5_02") ||
        rom_data.size() >= kSizeThreshold)
    {
        return rom_data;
    }
    if (rom_data.size() < kInsertionPoint)
    {
        rom_data.resize(kInsertionPoint, 0x00);
    }
    rom_data.insert(rom_data.begin() + static_cast<std::ptrdiff_t>(kInsertionPoint),
                    kPaddingBytes, 0xFF);
    return rom_data;
}

namespace
{

constexpr char kHexDigits[] = "0123456789abcdef";

std::uint32_t parse_hex_uint32(std::string_view text)
{
    if (text.empty())
    {
        return 0;
    }
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        // Whole-string check: std::from_chars stops at the first character
        // it cannot consume and reports success for the prefix, so without
        // this, "10junk" would parse as 0x10. Qt's toUInt(&ok, 16) -- what
        // legacy used -- rejects any trailing garbage, so rejecting it here
        // (via this function's existing "unparseable -> 0" fallback) is the
        // closer match.
        return 0;
    }
    return value;
}

std::string format_like_qt_g(double value, int precision)
{
    if (value == 0.0)
    {
        value = 0.0; // normalizes -0.0 to +0.0, matching Qt's QString::number
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    return std::string(buffer);
}

std::uint32_t storage_byte_size(std::string_view storage_type)
{
    if (storage_type == "uint16" || storage_type == "int16")
    {
        return 2;
    }
    if (storage_type == "uint24" || storage_type == "int24")
    {
        return 3;
    }
    if (storage_type == "uint32" || storage_type == "int32" || storage_type == "float")
    {
        return 4;
    }
    return 1;
}

std::uint32_t assemble_msb_first(const std::uint8_t *bytes, std::uint32_t count)
{
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        value = (value << 8) | bytes[i];
    }
    return value;
}

std::int32_t sign_extend(std::uint32_t value, std::uint32_t byte_count)
{
    const std::uint32_t bits = byte_count * 8;
    if (bits < 32 && (value & (std::uint32_t{1} << (bits - 1))) != 0)
    {
        value |= (~std::uint32_t{0} << bits);
    }
    return static_cast<std::int32_t>(value);
}

std::string join_with_trailing_comma(const std::vector<std::string>& values)
{
    std::string result;
    for (const std::string& value : values)
    {
        result += value;
        result += ",";
    }
    return result;
}

} // namespace

Result<std::string> decode_scaled_values(
    std::span<const std::uint8_t> rom_data,
    std::uint64_t base_address,
    std::uint32_t count,
    std::string_view start_position,
    std::string_view interval,
    std::string_view storage_type,
    std::string_view endian,
    std::string_view from_byte_expression,
    bool is_selectable,
    bool apply_wrx02_wraparound,
    int float_precision)
{
    const std::uint32_t storage_size = storage_byte_size(storage_type);
    const bool is_float = storage_type == "float";
    const bool is_uint = storage_type.starts_with("uint");
    const std::uint32_t start_pos = parse_hex_uint32(start_position);
    const std::uint32_t interval_value = parse_hex_uint32(interval);

    std::string result;
    for (std::uint32_t j = 0; j < count; ++j)
    {
        const std::uint32_t offset =
            j * storage_size * interval_value + (start_pos - 1) * storage_size;
        std::uint64_t byte_address = base_address + offset;

        if (apply_wrx02_wraparound && rom_data.size() < byte_address)
        {
            byte_address -= 0x8000;
        }

        if (byte_address + storage_size > rom_data.size())
        {
            return fail(ErrorKind::Internal,
                        "decode_scaled_values: byte address exceeds ROM size");
        }

        std::uint8_t normalized[4] = {0, 0, 0, 0};
        for (std::uint32_t k = 0; k < storage_size; ++k)
        {
            const std::uint8_t byte = rom_data[byte_address + k];
            if (endian == "little")
            {
                normalized[storage_size - 1 - k] = byte;
            }
            else
            {
                normalized[k] = byte;
            }
        }

        double value = 0.0;
        if (!is_selectable)
        {
            if (is_float)
            {
                const std::uint32_t bits = assemble_msb_first(normalized, storage_size);
                float float_value;
                std::memcpy(&float_value, &bits, sizeof(float_value));
                value = expression_evaluate(std::string(from_byte_expression),
                                            format_like_qt_g(float_value, float_precision),
                                            float_precision);
            }
            else if (is_uint)
            {
                const std::uint32_t int_value = assemble_msb_first(normalized, storage_size);
                value = expression_evaluate(std::string(from_byte_expression),
                                            std::to_string(int_value), float_precision);
            }
            else
            {
                const std::uint32_t raw_value = assemble_msb_first(normalized, storage_size);
                const std::int32_t signed_value = sign_extend(raw_value, storage_size);
                value = expression_evaluate(std::string(from_byte_expression),
                                            std::to_string(signed_value), float_precision);
            }
        }
        result += format_like_qt_g(value, float_precision);
        result += ",";
    }
    return result;
}

Result<std::string> decode_bloblist_hex(
    std::span<const std::uint8_t> rom_data,
    std::uint64_t address,
    std::uint32_t byte_count)
{
    if (address + byte_count > rom_data.size())
    {
        return fail(ErrorKind::Internal, "decode_bloblist_hex: address exceeds ROM size");
    }
    std::string result;
    result.reserve(byte_count * 2);
    for (std::uint32_t i = 0; i < byte_count; ++i)
    {
        const std::uint8_t byte = rom_data[address + i];
        result += kHexDigits[(byte >> 4) & 0xF];
        result += kHexDigits[byte & 0xF];
    }
    return result;
}

namespace
{

// Decodes one map's cells and axes. Every failure path returns an error for
// this map alone; compute_map_cell_values below turns that into a
// placeholder entry and moves on to the next map.
Result<MapCellValues> compute_one_map_cell_values(
    const definition::RomDefinition& rom_definition,
    const definition::CalibrationMap& map,
    std::span<const std::uint8_t> rom_data,
    bool apply_wrx02,
    int float_precision)
{
    MapCellValues values;
    const definition::Scaling *scaling =
        definition::find_scaling(rom_definition, map.scaling_name);

    if (map.storage_type == "bloblist")
    {
        const std::uint32_t byte_count =
            (scaling != nullptr && !scaling->selections.empty())
                ? static_cast<std::uint32_t>(scaling->selections.front().second.size() / 2)
                : 0;
        if (!map.address.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "bloblist map has no address");
        }
        auto decoded = decode_bloblist_hex(rom_data, *map.address, byte_count);
        if (!decoded.has_value())
        {
            return std::unexpected(decoded.error());
        }
        values.map_data = *decoded;
        return values;
    }

    // Resolved after the bloblist branch above returns: bloblist maps read
    // raw bytes and never evaluate a from_byte expression.
    const std::string from_byte = scaling != nullptr ? scaling->from_byte : std::string("x");

    if (!map.address.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "map has no address");
    }
    auto decoded = decode_scaled_values(
        rom_data, *map.address, map.x_size * map.y_size, map.start_position,
        map.interval, map.storage_type, map.endian, from_byte,
        map.type == "Selectable", apply_wrx02, float_precision);
    if (!decoded.has_value())
    {
        return std::unexpected(decoded.error());
    }
    values.map_data = *decoded;

    if (map.x_size > 1)
    {
        if (map.x_axis.type == "Static Y Axis" || map.x_axis.type == "Static X Axis")
        {
            values.x_axis_data = join_with_trailing_comma(map.x_axis.static_data);
        }
        else if (map.x_axis.type == "X Axis" ||
                 (map.x_axis.type == "Y Axis" && map.type == "2D"))
        {
            if (!map.x_axis.address.has_value())
            {
                return fail(ErrorKind::InvalidConfig, "x axis has no address");
            }
            const definition::Scaling *x_scaling =
                definition::find_scaling(rom_definition, map.x_axis.scaling_name);
            const std::string x_from_byte =
                x_scaling != nullptr ? x_scaling->from_byte : std::string("x");
            auto x_decoded = decode_scaled_values(
                rom_data, *map.x_axis.address, map.x_size, map.x_axis.start_position,
                map.x_axis.interval, map.x_axis.storage_type, map.x_axis.endian,
                x_from_byte, map.x_axis.type == "Selectable", apply_wrx02,
                float_precision);
            if (!x_decoded.has_value())
            {
                return std::unexpected(x_decoded.error());
            }
            values.x_axis_data = *x_decoded;
        }
    }

    if (map.y_size > 1)
    {
        if (!map.y_axis.address.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "y axis has no address");
        }
        const definition::Scaling *y_scaling =
            definition::find_scaling(rom_definition, map.y_axis.scaling_name);
        const std::string y_from_byte =
            y_scaling != nullptr ? y_scaling->from_byte : std::string("x");
        auto y_decoded = decode_scaled_values(
            rom_data, *map.y_axis.address, map.y_size, map.y_axis.start_position,
            map.y_axis.interval, map.y_axis.storage_type, map.y_axis.endian,
            y_from_byte, map.y_axis.type == "Selectable", apply_wrx02, float_precision);
        if (!y_decoded.has_value())
        {
            return std::unexpected(y_decoded.error());
        }
        values.y_axis_data = *y_decoded;
    }

    return values;
}

} // namespace

MapCellValuesList compute_map_cell_values(
    const definition::RomDefinition& rom_definition,
    std::span<const std::uint8_t> rom_data,
    std::string_view flash_method,
    int float_precision)
{
    MapCellValuesList result;
    result.reserve(rom_definition.maps.size());
    const bool apply_wrx02 = flash_method == "wrx02";

    for (const definition::CalibrationMap& map : rom_definition.maps)
    {
        Result<MapCellValues> values = compute_one_map_cell_values(
            rom_definition, map, rom_data, apply_wrx02, float_precision);
        if (!values.has_value())
        {
            MapCellValues placeholder;
            placeholder.error = values.error();
            result.push_back(std::move(placeholder));
            continue;
        }
        result.push_back(std::move(*values));
    }
    return result;
}

} // namespace fastecu::calibration
