#include "src/backend/calibration/calibration_service.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "src/algorithms/expression/expression_evaluator.h"

namespace fastecu::calibration
{
namespace
{

Status validate_extent(
    std::optional<std::uint64_t> address,
    std::uint32_t count,
    std::uint32_t start_position,
    std::uint32_t interval,
    std::optional<definition::StorageType> storage_type,
    const definition::Scaling *scaling,
    std::size_t rom_byte_length,
    std::string_view context)
{
    if (!address.has_value())
    {
        return {};
    }
    const std::uint32_t width = element_byte_size(storage_type, scaling);
    const std::uint64_t end = element_run_end(*address, start_position, interval, width, count);
    if (end > rom_byte_length)
    {
        return fail(ErrorKind::InvalidConfig, std::string(context) + " address exceeds ROM size");
    }
    return {};
}

constexpr char kHexDigits[] = "0123456789abcdef";

// Reproduces QString::number(value, 'g', precision). Qt's 'g' and C's %g agree
// on trailing-zero stripping and exponent thresholds across the range these
// ROMs produce -- pinned by FormattingMatchesCapturedQtGroundTruth, which
// compares against real Qt output rather than assuming compatibility.
std::string format_like_qt_g(double value, int precision)
{
    if (value == 0.0)
    {
        value = 0.0; // normalizes -0.0 to +0.0, as Qt does
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    return std::string(buffer);
}

// Sign-extends an assembled `width`-byte value to a full int32. Widths of 4 or
// more are already full-width; width 0 cannot occur (storage_byte_size floors
// at 1) but is handled rather than shifted out of range.
std::int32_t sign_extend(std::uint32_t raw, std::uint32_t width)
{
    if (width == 0 || width >= 4)
    {
        return static_cast<std::int32_t>(raw);
    }
    const std::uint32_t sign_bit = 1u << (width * 8 - 1);
    if ((raw & sign_bit) == 0)
    {
        return static_cast<std::int32_t>(raw);
    }
    return static_cast<std::int32_t>(raw | ~((sign_bit << 1) - 1));
}

std::string_view scaling_from_byte(const definition::Scaling *scaling)
{
    return scaling != nullptr ? std::string_view(scaling->from_byte) : std::string_view("x");
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

ElementRun map_element_run(const definition::CalibrationMap& map,
                           const definition::Scaling *scaling)
{
    return ElementRun{
        .address = map.address.value_or(0),
        .count = map.x_size * map.y_size,
        .start_position = map.start_position,
        .interval = map.interval,
        .storage_type = map.storage_type,
        .endian = map.endian,
        .from_byte = scaling_from_byte(scaling),
        .is_selectable = map.type == "Selectable",
    };
}

ElementRun axis_element_run(const definition::AxisDefinition& axis,
                            std::uint32_t count,
                            const definition::Scaling *scaling)
{
    return ElementRun{
        .address = axis.address.value_or(0),
        .count = count,
        .start_position = axis.start_position,
        .interval = axis.interval,
        .storage_type = axis.storage_type,
        .endian = axis.endian,
        .from_byte = scaling_from_byte(scaling),
        .is_selectable = axis.type == "Selectable",
    };
}

Result<MapCellValues> compute_one_map(const definition::RomDefinition& rom_definition,
                                      const definition::CalibrationMap& map,
                                      bytes::ByteView rom_data,
                                      int float_precision)
{
    MapCellValues values;
    const definition::Scaling *scaling =
        definition::find_scaling(rom_definition, map.scaling_name);

    if (!map.address.has_value())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("map '{}' has no address", map.name));
    }

    if (map.storage_type == definition::StorageType::Bloblist)
    {
        const std::uint32_t byte_count = element_byte_size(map.storage_type, scaling);
        auto decoded = decode_bloblist_hex(rom_data, *map.address, byte_count);
        if (!decoded.has_value())
        {
            return std::unexpected(decoded.error());
        }
        values.map_data = std::move(*decoded);
        return values;
    }

    auto cells = decode_scaled_values(rom_data, map_element_run(map, scaling), float_precision);
    if (!cells.has_value())
    {
        return std::unexpected(cells.error());
    }
    values.map_data = std::move(*cells);

    if (map.x_size > 1)
    {
        const definition::AxisDefinition& x_axis = map.x_axis;
        const definition::Scaling *x_scaling =
            definition::find_scaling(rom_definition, x_axis.scaling_name);
        if (x_axis.type == "Static X Axis" || x_axis.type == "Static Y Axis")
        {
            values.x_axis_data = join_with_trailing_comma(x_axis.static_data);
        }
        else if (x_axis.type == "X Axis" || (x_axis.type == "Y Axis" && map.type == "2D"))
        {
            auto decoded = decode_scaled_values(
                rom_data, axis_element_run(x_axis, map.x_size, x_scaling), float_precision);
            if (!decoded.has_value())
            {
                return std::unexpected(decoded.error());
            }
            values.x_axis_data = std::move(*decoded);
        }
        // Any other type: left at its " " default, not computed -- legacy did
        // not touch XScaleData in that case either.
    }

    if (map.y_size > 1)
    {
        // No type branching, deliberately: see the header.
        const definition::Scaling *y_scaling =
            definition::find_scaling(rom_definition, map.y_axis.scaling_name);
        auto decoded = decode_scaled_values(
            rom_data, axis_element_run(map.y_axis, map.y_size, y_scaling), float_precision);
        if (!decoded.has_value())
        {
            return std::unexpected(decoded.error());
        }
        values.y_axis_data = std::move(*decoded);
    }

    return values;
}

} // namespace

Result<std::vector<std::uint8_t>> read_rom(std::string_view file_handle,
                                           IFileRepository& file_repository)
{
    return file_repository.read(file_handle);
}

void backup_rom(std::span<const std::uint8_t> rom_data, std::string_view backup_handle,
                IFileRepository& file_repository)
{
    // Fire-and-forget, matching open_subaru_rom_file's own behavior of never
    // checking this write's result.
    (void)file_repository.write(backup_handle, rom_data);
}

std::uint32_t element_byte_size(
    std::optional<definition::StorageType> storage_type,
    const definition::Scaling *scaling)
{
    if (storage_type == definition::StorageType::Bloblist && scaling != nullptr &&
        !scaling->selections.empty())
    {
        return static_cast<std::uint32_t>(scaling->selections.front().second.size() / 2);
    }
    return definition::storage_byte_size(storage_type);
}

std::uint64_t element_run_end(
    std::uint64_t address,
    std::uint32_t start_position,
    std::uint32_t interval,
    std::uint32_t element_width,
    std::uint32_t count)
{
    if (count == 0)
    {
        // No elements laid out at all, so nothing past `address` is touched.
        // Without this, count - 1 wraps to 0xFFFFFFFF in uint32 arithmetic.
        return address;
    }
    // start_position is 1-based. 0 is out of domain and unvalidated upstream
    // (see the header); clamp it to the smallest legal value instead of
    // letting start_position - 1 wrap to 0xFFFFFFFF.
    const std::uint64_t start_offset =
        start_position == 0 ? 0 : std::uint64_t(start_position - 1);
    return address + start_offset * element_width +
           std::uint64_t(count - 1) * interval * element_width + element_width;
}

Status validate_rom_size(const definition::RomDefinition& rom_definition,
                         std::size_t rom_byte_length)
{
    for (const definition::CalibrationMap& map : rom_definition.maps)
    {
        if (auto status = validate_extent(
                map.address, map.x_size * map.y_size, map.start_position, map.interval,
                map.storage_type, definition::find_scaling(rom_definition, map.scaling_name),
                rom_byte_length, "map");
            !status.has_value())
        {
            return status;
        }
        if (auto status = validate_extent(
                map.x_axis.address, map.x_axis.size, map.x_axis.start_position,
                map.x_axis.interval, map.x_axis.storage_type,
                definition::find_scaling(rom_definition, map.x_axis.scaling_name), rom_byte_length,
                "x-axis");
            !status.has_value())
        {
            return status;
        }
        if (auto status = validate_extent(
                map.y_axis.address, map.y_axis.size, map.y_axis.start_position,
                map.y_axis.interval, map.y_axis.storage_type,
                definition::find_scaling(rom_definition, map.y_axis.scaling_name), rom_byte_length,
                "y-axis");
            !status.has_value())
        {
            return status;
        }
    }
    return {};
}

std::vector<std::uint8_t> apply_flash_method_padding(
    std::vector<std::uint8_t> rom_data, std::string_view flash_method)
{
    constexpr std::size_t kInsertAt = 0x20000;
    constexpr std::size_t kPadBytes = 0x8000;
    constexpr std::size_t kSizeThreshold = static_cast<std::size_t>(190) * 1024;

    if (!flash_method.starts_with("sub_ecu_denso_mc68hc16y5_02") ||
        rom_data.size() >= kSizeThreshold)
    {
        return rom_data;
    }
    if (rom_data.size() < kInsertAt)
    {
        rom_data.resize(kInsertAt, 0x00);
    }
    rom_data.insert(rom_data.begin() + static_cast<std::ptrdiff_t>(kInsertAt),
                    kPadBytes, 0xFF);
    return rom_data;
}

Result<std::string> decode_scaled_values(bytes::ByteView rom_data,
                                         const ElementRun& run,
                                         int float_precision)
{
    const std::uint32_t width = definition::storage_byte_size(run.storage_type);
    const bool is_float = run.storage_type == definition::StorageType::Float;
    const bool is_unsigned = definition::is_unsigned_storage(run.storage_type);
    const bool little_endian = run.endian == "little";
    // start_position is 1-based. definition_resolver rejects 0, but clamp
    // identically to element_run_end's guard so the two cannot disagree if
    // this is reached directly.
    const std::uint64_t start_offset =
        run.start_position == 0 ? 0 : std::uint64_t(run.start_position - 1);

    std::string result;
    for (std::uint32_t j = 0; j < run.count; ++j)
    {
        const std::uint64_t byte_address = run.address + start_offset * width +
                                           std::uint64_t(j) * width * run.interval;
        if (byte_address + width > rom_data.size())
        {
            return fail(ErrorKind::Internal,
                        std::format("element {} at 0x{:x} runs past ROM size {}",
                                    j, byte_address, rom_data.size()));
        }

        const std::uint32_t raw =
            little_endian
                ? bytes::readULe(rom_data, static_cast<std::size_t>(byte_address), width)
                : bytes::readUBe(rom_data, static_cast<std::size_t>(byte_address), width);

        double value = 0.0;
        if (!run.is_selectable)
        {
            if (is_float)
            {
                float float_value = 0.0F;
                std::memcpy(&float_value, &raw, sizeof(float_value));
                value = expression_evaluate(
                    run.from_byte, format_like_qt_g(float_value, float_precision),
                    float_precision);
            }
            else if (is_unsigned)
            {
                value = expression_evaluate(run.from_byte, std::to_string(raw),
                                            float_precision);
            }
            else
            {
                value = expression_evaluate(
                    run.from_byte, std::to_string(sign_extend(raw, width)),
                    float_precision);
            }
        }
        result += format_like_qt_g(value, float_precision);
        result += ",";
    }
    return result;
}

Result<std::string> decode_bloblist_hex(bytes::ByteView rom_data,
                                        std::uint64_t address,
                                        std::uint32_t byte_count)
{
    if (address + byte_count > rom_data.size())
    {
        return fail(ErrorKind::Internal,
                    std::format("bloblist at 0x{:x} ({} bytes) runs past ROM size {}",
                                address, byte_count, rom_data.size()));
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(byte_count) * 2);
    for (std::uint32_t i = 0; i < byte_count; ++i)
    {
        const std::uint8_t byte = rom_data[static_cast<std::size_t>(address) + i];
        result += kHexDigits[(byte >> 4) & 0x0F];
        result += kHexDigits[byte & 0x0F];
    }
    return result;
}

Result<MapCellValuesList> compute_map_cell_values(
    const definition::RomDefinition& rom_definition,
    bytes::ByteView rom_data,
    int float_precision)
{
    MapCellValuesList results;
    results.reserve(rom_definition.maps.size());
    for (const definition::CalibrationMap& map : rom_definition.maps)
    {
        auto computed = compute_one_map(rom_definition, map, rom_data, float_precision);
        if (computed.has_value())
        {
            results.push_back(std::move(*computed));
        }
        else
        {
            // One bad map degrades alone. Blanking every map in the ROM
            // because of a single bad entry would be a worse failure than the
            // one being reported.
            MapCellValues failed;
            failed.error = computed.error();
            results.push_back(std::move(failed));
        }
    }
    return results;
}

} // namespace fastecu::calibration
