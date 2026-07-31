#include "src/backend/calibration/calibration_service.h"

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

} // namespace fastecu::calibration
