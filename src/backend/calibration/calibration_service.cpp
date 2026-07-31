#include "src/backend/calibration/calibration_service.h"

#include <string>
#include <string_view>

namespace fastecu::calibration
{
namespace
{

const definition::Scaling *find_scaling(
    const definition::RomDefinition& rom_definition, std::string_view name)
{
    for (const definition::Scaling& scaling : rom_definition.scalings)
    {
        if (scaling.name == name)
        {
            return &scaling;
        }
    }
    return nullptr;
}

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

} // namespace

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
    return address + std::uint64_t(start_position - 1) * element_width +
           std::uint64_t(count - 1) * interval * element_width + element_width;
}

Status validate_rom_size(const definition::RomDefinition& rom_definition,
                         std::size_t rom_byte_length)
{
    for (const definition::CalibrationMap& map : rom_definition.maps)
    {
        if (auto status = validate_extent(
                map.address, map.x_size * map.y_size, map.start_position, map.interval,
                map.storage_type, find_scaling(rom_definition, map.scaling_name),
                rom_byte_length, "map");
            !status.has_value())
        {
            return status;
        }
        if (auto status = validate_extent(
                map.x_axis.address, map.x_axis.size, map.x_axis.start_position,
                map.x_axis.interval, map.x_axis.storage_type,
                find_scaling(rom_definition, map.x_axis.scaling_name), rom_byte_length,
                "x-axis");
            !status.has_value())
        {
            return status;
        }
        if (auto status = validate_extent(
                map.y_axis.address, map.y_axis.size, map.y_axis.start_position,
                map.y_axis.interval, map.y_axis.storage_type,
                find_scaling(rom_definition, map.y_axis.scaling_name), rom_byte_length,
                "y-axis");
            !status.has_value())
        {
            return status;
        }
    }
    return {};
}

} // namespace fastecu::calibration
