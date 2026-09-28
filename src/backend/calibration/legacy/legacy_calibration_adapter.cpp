#include "src/backend/calibration/legacy/legacy_calibration_adapter.h"

#include <format>
#include <string>

#include <QFileInfo>
#include <QDateTime>

#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/definition/definition_model.h"

namespace fastecu::calibration
{
namespace
{

// Matches FileActions::float_precision (file_actions.h:63), which is the
// precision legacy formatted every decoded cell with.
constexpr int kFloatPrecision = 15;

} // namespace

LegacyCalibrationAdapter::LegacyCalibrationAdapter(IFileRepository& file_repository) : file_repository_(file_repository)
{
}

Status LegacyCalibrationAdapter::open_rom_bytes(definitions::EcuCalDefStructure& ecu_cal_def, QString filename,
                                                std::string_view calibration_files_directory)
{
    const bool already_loaded = ecu_cal_def.FullRomData.length() > 0;

    if (already_loaded)
    {
        // The image is already in FullRomData (e.g. just read off the ECU);
        // only back it up. Nothing is copied out of it and nothing is
        // assigned back into it -- for a 512 KB-2 MB ROM that copy was pure
        // waste.
        if (filename.isEmpty())
        {
            filename = "read_image_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh'h'mm'm'ss's'") + ".bin";
        }
        const std::string backup_handle = std::string(calibration_files_directory) + "read.bin";
        backup_rom(bytes::view(ecu_cal_def.FullRomData), backup_handle, file_repository_);
    }
    else
    {
        if (filename.isEmpty())
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                 "open_rom_bytes called with no filename and no preloaded bytes");
        }
        Result<std::vector<std::uint8_t>> rom_data = read_rom(filename.toStdString(), file_repository_);
        if (!rom_data.has_value())
        {
            return std::unexpected(rom_data.error());
        }
        ecu_cal_def.FullRomData = bytes::toQByteArray(bytes::ByteView{*rom_data});
    }

    QFileInfo file_info(filename);
    QString file_name_str = file_info.fileName();
    if (file_name_str.isEmpty())
    {
        file_name_str = "default.bin";
    }
    ecu_cal_def.FileName = file_name_str;
    ecu_cal_def.FullFileName = filename;
    return {};
}

void LegacyCalibrationAdapter::apply_flash_method_padding(definitions::EcuCalDefStructure& ecu_cal_def,
                                                          const QString& flash_method)
{
    std::vector<std::uint8_t> rom_data(ecu_cal_def.FullRomData.cbegin(), ecu_cal_def.FullRomData.cend());
    rom_data = fastecu::calibration::apply_flash_method_padding(std::move(rom_data), flash_method.toStdString());
    ecu_cal_def.FullRomData = bytes::toQByteArray(bytes::ByteView(rom_data));
}

Status LegacyCalibrationAdapter::compute_map_cell_values(definitions::EcuCalDefStructure& ecu_cal_def,
                                                         const definition::RomDefinition& rom_definition)
{
    auto computed = fastecu::calibration::compute_map_cell_values(rom_definition, bytes::view(ecu_cal_def.FullRomData),
                                                                  kFloatPrecision);
    if (!computed.has_value())
    {
        return std::unexpected(computed.error());
    }
    if (static_cast<qsizetype>(computed->size()) != ecu_cal_def.MapData.size())
    {
        return fail(ErrorKind::Internal,
                    std::format("definition has {} maps but legacy columns hold {}", computed->size(),
                                static_cast<std::size_t>(ecu_cal_def.MapData.size())));
    }

    std::string first_error;
    for (std::size_t index = 0; index < computed->size(); ++index)
    {
        const MapCellValues& values = computed->at(index);
        if (values.error.has_value())
        {
            if (first_error.empty())
            {
                first_error = values.error->detail;
            }
            continue;
        }
        const auto legacy_index = static_cast<qsizetype>(index);
        ecu_cal_def.MapData.replace(legacy_index, QString::fromStdString(values.map_data));
        ecu_cal_def.XScaleData.replace(legacy_index, QString::fromStdString(values.x_axis_data));
        ecu_cal_def.YScaleData.replace(legacy_index, QString::fromStdString(values.y_axis_data));
    }
    if (!first_error.empty())
    {
        return fail(ErrorKind::Internal, first_error);
    }
    return {};
}

definitions::EcuCalDefStructure *
LegacyCalibrationAdapter::save_subaru_rom_file(definitions::EcuCalDefStructure *ecu_cal_def, const QString& filename)
{
    // Straight to the repository: there is no save-side policy for a
    // calibration_service function to carry, unlike the open path's
    // fire-and-forget backup_rom.
    const Status result = file_repository_.write(filename.toStdString(), bytes::view(ecu_cal_def->FullRomData));
    if (!result.has_value())
    {
        return nullptr;
    }
    QFileInfo file_info(filename);
    ecu_cal_def->FullFileName = filename;
    ecu_cal_def->FileName = file_info.fileName();
    return ecu_cal_def;
}

} // namespace fastecu::calibration
