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

} // namespace fastecu::calibration
