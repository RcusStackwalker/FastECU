#include "src/backend/calibration/legacy_calibration_adapter.h"

#include <QFileInfo>
#include <QDateTime>

#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/config/protocol_catalog.h"

namespace fastecu::calibration
{
namespace
{

// Mirrors legacy_config_adapter.cpp's own (private, anonymous-namespace)
// paths_from_legacy -- duplicated here deliberately rather than exported
// from that already-merged file, matching this slice's accepted-cost choice
// of re-parsing protocols.cfg rather than sharing 5d-1's cached catalogs.
config::ConfigPaths paths_from_config_values(
    const definitions::ConfigValuesStructure& values)
{
    config::ConfigPaths paths;
    paths.base_config_directory = values.base_config_directory.toStdString();
    paths.version_config_directory = values.version_config_directory.toStdString();
    paths.calibration_files_directory = values.calibration_files_directory.toStdString();
    paths.config_files_directory = values.config_files_directory.toStdString();
    paths.definition_files_directory = values.definition_files_directory.toStdString();
    paths.kernel_files_directory = values.kernel_files_directory.toStdString();
    paths.datalog_files_directory = values.datalog_files_directory.toStdString();
    paths.syslog_files_directory = values.syslog_files_directory.toStdString();
    paths.config_file = values.config_file.toStdString();
    paths.menu_file = values.menu_file.toStdString();
    paths.protocols_file = values.protocols_file.toStdString();
    paths.logger_file = values.logger_file.toStdString();
    return paths;
}

} // namespace

LegacyCalibrationAdapter::LegacyCalibrationAdapter(IFileRepository& file_repository)
    : file_repository_(file_repository)
{
}

Status LegacyCalibrationAdapter::open_rom_bytes(
    definitions::EcuCalDefStructure& ecu_cal_def, QString filename,
    const definitions::ConfigValuesStructure& config_values)
{
    const bool already_loaded = ecu_cal_def.FullRomData.length() > 0;

    if (already_loaded)
    {
        if (filename.isEmpty())
        {
            filename = "read_image_" +
                       QDateTime::currentDateTime().toString("yyyy-MM-dd_hh'h'mm'm'ss's'") +
                       ".bin";
        }
    }
    else if (filename.isEmpty())
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "open_rom_bytes called with no filename and no preloaded bytes");
    }

    const std::string backup_handle =
        (config_values.calibration_files_directory + "read.bin").toStdString();
    const bytes::ByteView preloaded =
        already_loaded ? bytes::view(ecu_cal_def.FullRomData) : bytes::ByteView{};
    Result<std::vector<std::uint8_t>> result =
        open_rom(filename.toStdString(), preloaded, backup_handle, file_repository_);
    if (!result.has_value())
    {
        return std::unexpected(result.error());
    }
    if (!already_loaded)
    {
        ecu_cal_def.FullRomData =
            bytes::toQByteArray(bytes::ByteView(result->data(), result->size()));
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

void LegacyCalibrationAdapter::bind_protocol(
    definitions::ConfigValuesStructure& config_values, const QString& flash_method)
{
    const config::ConfigPaths paths = paths_from_config_values(config_values);
    Result<config::ProtocolCatalog> protocols =
        config::load_protocol_catalog(paths, file_repository_);
    Result<config::CarModelCatalog> car_models =
        config::load_car_model_catalog(paths, file_repository_);
    if (!protocols.has_value() || !car_models.has_value())
    {
        return;
    }

    const std::vector<config::ResolvedCarModel> resolved =
        config::resolve_car_models(*protocols, *car_models);
    const std::optional<std::size_t> index =
        config::find_car_model_by_protocol_name(resolved, flash_method.toStdString());
    if (!index.has_value())
    {
        return;
    }

    const config::ResolvedCarModel& row = resolved[*index];
    config_values.flash_protocol_selected_id = QString::number(*index);
    config_values.flash_protocol_selected_make = QString::fromStdString(row.make);
    config_values.flash_protocol_selected_model = QString::fromStdString(row.model);
    config_values.flash_protocol_selected_version = QString::fromStdString(row.version);
    config_values.flash_protocol_selected_protocol_name =
        QString::fromStdString(row.protocol_name);
    if (row.protocol.has_value())
    {
        config_values.flash_protocol_selected_description =
            QString::fromStdString(row.protocol->description);
        config_values.flash_protocol_selected_log_protocol =
            QString::fromStdString(row.protocol->log_protocol);
        config_values.flash_protocol_selected_mcu = QString::fromStdString(row.protocol->mcu);
        config_values.flash_protocol_selected_checksum =
            QString::fromStdString(row.protocol->checksum);
    }
}

definitions::EcuCalDefStructure *LegacyCalibrationAdapter::save_subaru_rom_file(
    definitions::EcuCalDefStructure *ecu_cal_def, const QString& filename)
{
    const Status result = save_rom(bytes::view(ecu_cal_def->FullRomData),
                                   filename.toStdString(), file_repository_);
    if (!result.has_value())
    {
        return nullptr;
    }
    QFileInfo file_info(filename);
    ecu_cal_def->FullFileName = filename;
    ecu_cal_def->FileName = file_info.fileName();
    return ecu_cal_def;
}

void LegacyCalibrationAdapter::compute_map_cell_values(
    definitions::EcuCalDefStructure& ecu_cal_def,
    const definition::RomDefinition& rom_definition, const QString& flash_method,
    int float_precision)
{
    const bytes::ByteView original = bytes::view(ecu_cal_def.FullRomData);
    std::vector<std::uint8_t> padded = apply_flash_method_padding(
        std::vector<std::uint8_t>(original.begin(), original.end()),
        flash_method.toStdString());

    // Qualified with calibration:: (unlike apply_flash_method_padding above,
    // which has no name clash and is called unqualified): this member
    // function shares its name with calibration_service.h's free function
    // of the same name. Unqualified lookup from inside a member function
    // body checks class scope before the enclosing namespace, so an
    // unqualified call here resolves to *this* member function (a
    // self-recursive, type-mismatched call that fails to compile) rather
    // than falling through to the free function -- verified directly by
    // compiling both the unqualified form (fails: "cannot initialize a
    // variable of type 'int' with an rvalue of type 'void'", i.e. it binds
    // to the member function itself) and this qualified form (compiles,
    // resolves to the free function) in isolated repros. Qualifying with
    // calibration:: is not a "self-referencing" lookup problem -- namespace
    // members remain reachable by qualifying with their own enclosing
    // namespace's name from inside that namespace, unlike the class-scope
    // hiding rule above.
    Result<MapCellValuesList> computed = calibration::compute_map_cell_values(
        rom_definition, padded, flash_method.toStdString(), float_precision);
    if (!computed.has_value())
    {
        return;
    }
    for (std::size_t i = 0; i < computed->size() && static_cast<int>(i) < ecu_cal_def.MapData.size(); ++i)
    {
        const MapCellValues& values = (*computed)[i];
        ecu_cal_def.MapData.replace(static_cast<int>(i), QString::fromStdString(values.map_data));
        ecu_cal_def.XScaleData.replace(static_cast<int>(i), QString::fromStdString(values.x_axis_data));
        ecu_cal_def.YScaleData.replace(static_cast<int>(i), QString::fromStdString(values.y_axis_data));
    }
}

} // namespace fastecu::calibration
