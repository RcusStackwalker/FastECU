#include "src/backend/calibration/calibration_service.h"

#include <bit>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>

#include "src/algorithms/expression/expression.h"
#include "src/backend/calibration/scaling_internal.h"

namespace fastecu::calibration
{
using namespace internal;

Result<NumericRun> DecodeNumericRun(bytes::ByteView rom, const ElementRun& run)
{
    if (!run.storage_type.has_value() || run.storage_type == definition::StorageType::kBloblist ||
        run.start_position == 0 || run.interval == 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "numeric run has invalid storage or stride metadata");
    }
    const auto width = definition::StorageByteSize(run.storage_type);
    const bool is_float = run.storage_type == definition::StorageType::kFloat;
    const bool little_endian = !is_float && run.endian == "little";
    const auto end = ElementRunEnd(run.address, run.start_position, run.interval, width, run.count);
    if (end > rom.size())
    {
        return Fail(ErrorKind::kInvalidConfig, "numeric run exceeds ROM size");
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
                               : definition::IsUnsignedStorage(run.storage_type)
                                   ? static_cast<double>(raw)
                                   : static_cast<double>(SignExtend(raw, width));
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
                Fail(ErrorKind::kInvalidConfig, std::format("cell {}: {}", index, value.error().detail)));
        }
    }
    return result;
}

namespace
{

Status ValidateExtent(std::optional<std::uint64_t> address, std::uint32_t count, std::uint32_t start_position,
                      std::uint32_t interval, std::optional<definition::StorageType> storage_type,
                      const definition::Scaling *scaling, std::size_t rom_byte_length, std::string_view context)
{
    if (!address.has_value())
    {
        return {};
    }
    const std::uint32_t width = ElementByteSize(storage_type, scaling);
    if (const std::uint64_t end = ElementRunEnd(*address, start_position, interval, width, count);
        end > rom_byte_length)
    {
        return Fail(ErrorKind::kInvalidConfig, std::string(context) + " address exceeds ROM size");
    }
    return {};
}

ElementRun MapElementRun(const definition::CalibrationMap& map, const definition::Scaling *scaling, std::uint32_t count)
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

ElementRun AxisElementRun(const definition::AxisDefinition& axis, std::uint32_t count)
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

Result<AxisValue> DecodeTypedAxis(const definition::AxisDefinition& axis, std::uint32_t extent, bytes::ByteView rom,
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
            return Fail(ErrorKind::kInvalidConfig, "static axis label count differs from map extent");
        }
        return AxisValue(StaticAxis{axis.static_data});
    }
    if (x_axis && axis.type != "X Axis" && !(axis.type == "Y Axis" && map_type == "2D"))
    {
        return AxisValue{};
    }
    if (!axis.address.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "numeric axis has no address");
    }
    auto values = DecodeNumericRun(rom, AxisElementRun(axis, extent));
    if (!values.has_value())
    {
        return std::unexpected(values.error());
    }
    return AxisValue(std::move(*values));
}

} // namespace

Result<DecodedMap> DecodeCalibrationMap(const definition::RomDefinition& rom_definition,
                                        const definition::CalibrationMap& map, bytes::ByteView rom)
{
    const auto *scaling = definition::FindScaling(rom_definition, map.scaling_name);
    if (!map.scaling_name.empty() && scaling == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("map '{}' has unresolved scaling", map.name));
    }
    const std::uint64_t count = std::uint64_t(map.x_size) * map.y_size;
    if (!map.address.has_value() || count == 0 || count > std::numeric_limits<std::uint32_t>::max())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("map '{}' has invalid address or dimensions", map.name));
    }
    DecodedMap result;
    const auto storage = map.storage_type.has_value() ? map.storage_type
                         : scaling != nullptr         ? scaling->storage_type
                                                      : std::nullopt;
    if (storage == definition::StorageType::kBloblist)
    {
        const auto width = ElementByteSize(storage, scaling);
        if (!ByteWindowFits(rom, *map.address, width))
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("map '{}' blob exceeds ROM size", map.name));
        }
        const auto data = rom.subspan(static_cast<std::size_t>(*map.address), width);
        result.body = BlobValue{bytes::Bytes(data.begin(), data.end())};
        return result;
    }
    auto run = MapElementRun(map, scaling, static_cast<std::uint32_t>(count));
    run.storage_type = storage;
    run.endian = !map.endian.empty()  ? std::string_view(map.endian)
                 : scaling != nullptr ? std::string_view(scaling->endian)
                                      : std::string_view{};
    run.from_byte = scaling != nullptr ? std::string_view(scaling->from_byte) : std::string_view("x");
    auto body = DecodeNumericRun(rom, run);
    if (!body.has_value())
    {
        return Fail(body.error().kind, std::format("map '{}': {}", map.name, body.error().detail));
    }
    result.body = std::move(*body);

    auto x_axis = DecodeTypedAxis(map.x_axis, map.x_size, rom, true, map.type);
    if (!x_axis.has_value())
    {
        return Fail(x_axis.error().kind, std::format("map '{}' X axis: {}", map.name, x_axis.error().detail));
    }
    auto y_axis = DecodeTypedAxis(map.y_axis, map.y_size, rom, false, map.type);
    if (!y_axis.has_value())
    {
        return Fail(y_axis.error().kind, std::format("map '{}' Y axis: {}", map.name, y_axis.error().detail));
    }
    result.x_axis = std::move(*x_axis);
    result.y_axis = std::move(*y_axis);
    return result;
}

Result<std::vector<std::uint8_t>> ReadRom(std::string_view file_handle, IFileRepository& file_repository)
{
    return file_repository.Read(file_handle);
}

void BackupRom(std::span<const std::uint8_t> rom_data, std::string_view backup_handle, IFileRepository& file_repository)
{
    // Fire-and-forget, matching open_subaru_rom_file's own behavior of never
    // checking this write's result.
    std::ignore = file_repository.Write(backup_handle, rom_data);
}

std::uint32_t ElementByteSize(std::optional<definition::StorageType> storage_type, const definition::Scaling *scaling)
{
    if (storage_type == definition::StorageType::kBloblist && scaling != nullptr && !scaling->selections.empty())
    {
        return static_cast<std::uint32_t>(scaling->selections.front().value.size());
    }
    return definition::StorageByteSize(storage_type);
}

std::uint64_t ElementRunEnd(std::uint64_t address, std::uint32_t start_position, std::uint32_t interval,
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
    if (!CheckedMultiply(start_offset, element_width, start_bytes) ||
        !CheckedMultiply(interval, element_width, stride) || !CheckedMultiply(count - 1, stride, last_offset) ||
        !CheckedAdd(address, start_bytes, end) || !CheckedAdd(end, last_offset, end) ||
        !CheckedAdd(end, element_width, end))
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return end;
}

Status ValidateRomSize(const definition::RomDefinition& rom_definition, std::size_t rom_byte_length)
{
    for (const definition::CalibrationMap& map : rom_definition.maps)
    {
        if (auto status =
                ValidateExtent(map.address, map.x_size * map.y_size, map.start_position, map.interval, map.storage_type,
                               definition::FindScaling(rom_definition, map.scaling_name), rom_byte_length, "map");
            !status.has_value())
        {
            return status;
        }
        if (auto status = ValidateExtent(map.x_axis.address, map.x_axis.size, map.x_axis.start_position,
                                         map.x_axis.interval, map.x_axis.storage_type,
                                         definition::FindScaling(rom_definition, map.x_axis.scaling_name),
                                         rom_byte_length, "x-axis");
            !status.has_value())
        {
            return status;
        }
        if (auto status = ValidateExtent(map.y_axis.address, map.y_axis.size, map.y_axis.start_position,
                                         map.y_axis.interval, map.y_axis.storage_type,
                                         definition::FindScaling(rom_definition, map.y_axis.scaling_name),
                                         rom_byte_length, "y-axis");
            !status.has_value())
        {
            return status;
        }
    }
    return {};
}

std::vector<std::uint8_t> ApplyFlashMethodPadding(std::vector<std::uint8_t> rom_data, std::string_view flash_method)
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
