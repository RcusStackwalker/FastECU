#include "src/backend/calibration/calibration_service.h"

#include <bit>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>

#include "src/algorithms/expression/checked_expression.h"
#include "src/backend/calibration/scaling_internal.h"

namespace fastecu::calibration
{
using namespace internal;

Result<NumericRun> decode_numeric_run(bytes::ByteView rom, const ElementRun& run)
{
    if (!run.storage_type.has_value() || run.storage_type == definition::StorageType::kBloblist ||
        run.start_position == 0 || run.interval == 0)
    {
        return fail(ErrorKind::kInvalidConfig, "numeric run has invalid storage or stride metadata");
    }
    const auto width = definition::storage_byte_size(run.storage_type);
    const bool is_float = run.storage_type == definition::StorageType::kFloat;
    const bool little_endian = !is_float && run.endian == "little";
    const auto end = element_run_end(run.address, run.start_position, run.interval, width, run.count);
    if (end > rom.size())
    {
        return fail(ErrorKind::kInvalidConfig, "numeric run exceeds ROM size");
    }
    NumericRun result;
    result.cells.reserve(run.count);
    for (std::uint32_t index = 0; index < run.count; ++index)
    {
        const auto address =
            run.address + std::uint64_t(run.start_position - 1) * width + std::uint64_t(index) * width * run.interval;
        const auto raw = little_endian ? bytes::ReadULe(rom, static_cast<std::size_t>(address), width)
                                       : bytes::ReadUBe(rom, static_cast<std::size_t>(address), width);
        const double numeric = is_float ? static_cast<double>(std::bit_cast<float>(raw))
                               : definition::is_unsigned_storage(run.storage_type)
                                   ? static_cast<double>(raw)
                                   : static_cast<double>(sign_extend(raw, width));
        // Numeric Selectable's existing control semantics are outside this
        // migration; blob selections are represented separately as bytes.
        const auto value = run.is_selectable ? std::expected<double, expression::EvaluationError>(0.0)
                                             : expression::EvaluateChecked(run.from_byte, numeric);
        if (value.has_value())
        {
            result.cells.emplace_back(*value);
        }
        else
        {
            result.cells.emplace_back(
                fail(ErrorKind::kInvalidConfig, std::format("cell {}: {}", index, value.error().detail)));
        }
    }
    return result;
}

namespace
{

Status validate_extent(std::optional<std::uint64_t> address, std::uint32_t count, std::uint32_t start_position,
                       std::uint32_t interval, std::optional<definition::StorageType> storage_type,
                       const definition::Scaling *scaling, std::size_t rom_byte_length, std::string_view context)
{
    if (!address.has_value())
    {
        return {};
    }
    const std::uint32_t width = element_byte_size(storage_type, scaling);
    if (const std::uint64_t end = element_run_end(*address, start_position, interval, width, count);
        end > rom_byte_length)
    {
        return fail(ErrorKind::kInvalidConfig, std::string(context) + " address exceeds ROM size");
    }
    return {};
}

ElementRun map_element_run(const definition::CalibrationMap& map, const definition::Scaling *scaling,
                           std::uint32_t count)
{
    return ElementRun{
        .address = map.address.value_or(0),
        .count = count,
        .start_position = map.start_position,
        .interval = map.interval,
        .storage_type = map.storage_type,
        .endian = map.endian,
        .from_byte = scaling != nullptr ? std::string_view(scaling->from_byte) : std::string_view("x"),
        .is_selectable = map.type == "Selectable",
    };
}

ElementRun axis_element_run(const definition::AxisDefinition& axis, std::uint32_t count)
{
    return ElementRun{
        .address = axis.address.value_or(0),
        .count = count,
        .start_position = axis.start_position,
        .interval = axis.interval,
        .storage_type = axis.storage_type,
        .endian = axis.endian,
        .from_byte = axis.from_byte,
        .is_selectable = axis.type == "Selectable",
    };
}

Result<AxisValue> decode_typed_axis(const definition::AxisDefinition& axis, std::uint32_t extent, bytes::ByteView rom,
                                    bool x_axis, std::string_view map_type)
{
    if (extent <= 1 || axis.type.empty())
    {
        return AxisValue{};
    }
    if (x_axis && (axis.type == "Static X Axis" || axis.type == "Static Y Axis"))
    {
        if (axis.static_data.size() != extent)
        {
            return fail(ErrorKind::kInvalidConfig, "static axis label count differs from map extent");
        }
        return AxisValue(StaticAxis{axis.static_data});
    }
    if (x_axis && axis.type != "X Axis" && !(axis.type == "Y Axis" && map_type == "2D"))
    {
        return AxisValue{};
    }
    if (!axis.address.has_value())
    {
        return fail(ErrorKind::kInvalidConfig, "numeric axis has no address");
    }
    auto values = decode_numeric_run(rom, axis_element_run(axis, extent));
    if (!values.has_value())
    {
        return std::unexpected(values.error());
    }
    return AxisValue(std::move(*values));
}

} // namespace

Result<DecodedMap> decode_calibration_map(const definition::RomDefinition& rom_definition,
                                          const definition::CalibrationMap& map, bytes::ByteView rom)
{
    const auto *scaling = definition::find_scaling(rom_definition, map.scaling_name);
    if (!map.scaling_name.empty() && scaling == nullptr)
    {
        return fail(ErrorKind::kInvalidConfig, std::format("map '{}' has unresolved scaling", map.name));
    }
    const std::uint64_t count = std::uint64_t(map.x_size) * map.y_size;
    if (!map.address.has_value() || count == 0 || count > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(ErrorKind::kInvalidConfig, std::format("map '{}' has invalid address or dimensions", map.name));
    }
    DecodedMap result;
    const auto storage = map.storage_type.has_value() ? map.storage_type
                         : scaling != nullptr         ? scaling->storage_type
                                                      : std::nullopt;
    if (storage == definition::StorageType::kBloblist)
    {
        const auto width = element_byte_size(storage, scaling);
        if (!byte_window_fits(rom, *map.address, width))
        {
            return fail(ErrorKind::kInvalidConfig, std::format("map '{}' blob exceeds ROM size", map.name));
        }
        const auto data = rom.subspan(static_cast<std::size_t>(*map.address), width);
        result.body = BlobValue{bytes::Bytes(data.begin(), data.end())};
        return result;
    }
    auto run = map_element_run(map, scaling, static_cast<std::uint32_t>(count));
    run.storage_type = storage;
    run.endian = !map.endian.empty()  ? std::string_view(map.endian)
                 : scaling != nullptr ? std::string_view(scaling->endian)
                                      : std::string_view{};
    run.from_byte = scaling != nullptr ? std::string_view(scaling->from_byte) : std::string_view("x");
    auto body = decode_numeric_run(rom, run);
    if (!body.has_value())
    {
        return fail(body.error().kind, std::format("map '{}': {}", map.name, body.error().detail));
    }
    result.body = std::move(*body);

    auto x_axis = decode_typed_axis(map.x_axis, map.x_size, rom, true, map.type);
    if (!x_axis.has_value())
    {
        return fail(x_axis.error().kind, std::format("map '{}' X axis: {}", map.name, x_axis.error().detail));
    }
    auto y_axis = decode_typed_axis(map.y_axis, map.y_size, rom, false, map.type);
    if (!y_axis.has_value())
    {
        return fail(y_axis.error().kind, std::format("map '{}' Y axis: {}", map.name, y_axis.error().detail));
    }
    result.x_axis = std::move(*x_axis);
    result.y_axis = std::move(*y_axis);
    return result;
}

Result<std::vector<std::uint8_t>> read_rom(std::string_view file_handle, IFileRepository& file_repository)
{
    return file_repository.read(file_handle);
}

void backup_rom(std::span<const std::uint8_t> rom_data, std::string_view backup_handle,
                IFileRepository& file_repository)
{
    // Fire-and-forget, matching open_subaru_rom_file's own behavior of never
    // checking this write's result.
    std::ignore = file_repository.write(backup_handle, rom_data);
}

std::uint32_t element_byte_size(std::optional<definition::StorageType> storage_type, const definition::Scaling *scaling)
{
    if (storage_type == definition::StorageType::kBloblist && scaling != nullptr && !scaling->selections.empty())
    {
        return static_cast<std::uint32_t>(scaling->selections.front().value.size());
    }
    return definition::storage_byte_size(storage_type);
}

std::uint64_t element_run_end(std::uint64_t address, std::uint32_t start_position, std::uint32_t interval,
                              std::uint32_t element_width, std::uint32_t count)
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
    const std::uint64_t start_offset = start_position == 0 ? 0 : std::uint64_t(start_position - 1);
    std::uint64_t start_bytes = 0;
    std::uint64_t stride = 0;
    std::uint64_t last_offset = 0;
    std::uint64_t end = 0;
    if (!checked_multiply(start_offset, element_width, start_bytes) ||
        !checked_multiply(interval, element_width, stride) || !checked_multiply(count - 1, stride, last_offset) ||
        !checked_add(address, start_bytes, end) || !checked_add(end, last_offset, end) ||
        !checked_add(end, element_width, end))
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return end;
}

Status validate_rom_size(const definition::RomDefinition& rom_definition, std::size_t rom_byte_length)
{
    for (const definition::CalibrationMap& map : rom_definition.maps)
    {
        if (auto status = validate_extent(map.address, map.x_size * map.y_size, map.start_position, map.interval,
                                          map.storage_type, definition::find_scaling(rom_definition, map.scaling_name),
                                          rom_byte_length, "map");
            !status.has_value())
        {
            return status;
        }
        if (auto status = validate_extent(map.x_axis.address, map.x_axis.size, map.x_axis.start_position,
                                          map.x_axis.interval, map.x_axis.storage_type,
                                          definition::find_scaling(rom_definition, map.x_axis.scaling_name),
                                          rom_byte_length, "x-axis");
            !status.has_value())
        {
            return status;
        }
        if (auto status = validate_extent(map.y_axis.address, map.y_axis.size, map.y_axis.start_position,
                                          map.y_axis.interval, map.y_axis.storage_type,
                                          definition::find_scaling(rom_definition, map.y_axis.scaling_name),
                                          rom_byte_length, "y-axis");
            !status.has_value())
        {
            return status;
        }
    }
    return {};
}

std::vector<std::uint8_t> apply_flash_method_padding(std::vector<std::uint8_t> rom_data, std::string_view flash_method)
{
    constexpr std::size_t kInsertAt = 0x20000;
    constexpr std::size_t kPadBytes = 0x8000;
    constexpr std::size_t kSizeThreshold = static_cast<std::size_t>(190) * 1024;

    if (!flash_method.starts_with("sub_ecu_denso_mc68hc16y5_02") || rom_data.size() >= kSizeThreshold)
    {
        return rom_data;
    }
    if (rom_data.size() < kInsertAt)
    {
        rom_data.resize(kInsertAt, 0x00);
    }
    rom_data.insert(rom_data.begin() + static_cast<std::ptrdiff_t>(kInsertAt), kPadBytes, 0xFF);
    return rom_data;
}

} // namespace fastecu::calibration
