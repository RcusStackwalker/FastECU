#include "src/backend/calibration/calibration_service.h"

#include <bit>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>

#include "src/algorithms/expression/expression.h"
#include "src/backend/calibration/definition_interpretation_internal.h"
#include "src/backend/calibration/scaling_internal.h"

namespace fastecu::calibration
{
using namespace internal;

Result<NumericRun> DecodeNumericRun(const DefinitionBytes& rom, const ElementRun& run)
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
    const std::optional<bytes::ByteView> window = rom.At(run.address, end - run.address);
    if (!window.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "numeric run exceeds ROM size");
    }
    NumericRun result;
    result.cells.reserve(run.count);
    for (std::uint32_t index = 0; index < run.count; ++index)
    {
        const auto address =
            run.address + std::uint64_t(run.start_position - 1) * width + std::uint64_t(index) * width * run.interval;
        const auto offset = static_cast<std::size_t>(address - run.address);
        const auto raw =
            little_endian ? bytes::ReadULe(*window, offset, width) : bytes::ReadUBe(*window, offset, width);
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

Result<AxisValue> DecodeTypedAxis(const definition::AxisDefinition& axis, std::uint32_t extent,
                                  const DefinitionBytes& rom, bool x_axis, std::string_view map_type,
                                  const MapElement& element)
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
    auto values = DecodeNumericRun(rom, element.Run());
    if (!values.has_value())
    {
        return std::unexpected(values.error());
    }
    return AxisValue(std::move(*values));
}

} // namespace

Result<DecodedMap> DecodeCalibrationMap(const definition::RomDefinition& rom_definition,
                                        const definition::CalibrationMap& map, const DefinitionBytes& rom)
{
    const auto *scaling = definition::FindScaling(rom_definition, map.scaling_name);
    if (!map.scaling_name.empty() && scaling == nullptr)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("map '{}' has unresolved scaling", map.name));
    }
    const auto element = ResolveMapElement(rom_definition, map, NumericTarget::kMapBody);
    if (!element.address.has_value() || element.count() == 0 ||
        element.count() > std::numeric_limits<std::uint32_t>::max())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("map '{}' has invalid address or dimensions", map.name));
    }
    DecodedMap result;
    if (element.storage_type == definition::StorageType::kBloblist)
    {
        const std::optional<bytes::ByteView> data = rom.At(element.address->Value(), element.byte_width);
        if (!data.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("map '{}' blob exceeds ROM size", map.name));
        }
        result.body = BlobValue{bytes::Bytes(data->begin(), data->end())};
        return result;
    }
    const auto run = element.Run();
    auto body = DecodeNumericRun(rom, run);
    if (!body.has_value())
    {
        return Fail(body.error().kind, std::format("map '{}': {}", map.name, body.error().detail));
    }
    result.body = std::move(*body);

    const auto x_element = ResolveMapElement(rom_definition, map, NumericTarget::kXAxis);
    auto x_axis = DecodeTypedAxis(map.x_axis, map.x_size, rom, true, map.type, x_element);
    if (!x_axis.has_value())
    {
        return Fail(x_axis.error().kind, std::format("map '{}' X axis: {}", map.name, x_axis.error().detail));
    }
    const auto y_element = ResolveMapElement(rom_definition, map, NumericTarget::kYAxis);
    auto y_axis = DecodeTypedAxis(map.y_axis, map.y_size, rom, false, map.type, y_element);
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

} // namespace fastecu::calibration
