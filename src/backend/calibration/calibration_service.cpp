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

std::uint32_t parse_hex_uint32(std::string_view text)
{
    if (text.empty())
    {
        return 0;
    }
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc{})
    {
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

} // namespace fastecu::calibration
