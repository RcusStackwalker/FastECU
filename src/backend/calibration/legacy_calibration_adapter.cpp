#include "src/backend/calibration/legacy_calibration_adapter.h"

#include <format>

#include <QFileInfo>
#include <QDateTime>

#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/config/protocol_catalog.h"
#include "src/backend/definition/definition_model.h"

namespace fastecu::calibration
{
namespace
{

// Matches FileActions::float_precision (file_actions.h:63), which is the
// precision legacy formatted every decoded cell with.
constexpr int kFloatPrecision = 15;

// Mirrors legacy_config_adapter.cpp's own (private, anonymous-namespace)
// paths_from_legacy -- duplicated here deliberately rather than exported
// from that already-merged file.
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
        // The image is already in FullRomData (e.g. just read off the ECU);
        // only back it up. Nothing is copied out of it and nothing is
        // assigned back into it -- for a 512 KB-2 MB ROM that copy was pure
        // waste.
        if (filename.isEmpty())
        {
            filename = "read_image_" +
                       QDateTime::currentDateTime().toString("yyyy-MM-dd_hh'h'mm'm'ss's'") +
                       ".bin";
        }
        const std::string backup_handle =
            (config_values.calibration_files_directory + "read.bin").toStdString();
        backup_rom(bytes::view(ecu_cal_def.FullRomData), backup_handle, file_repository_);
    }
    else
    {
        if (filename.isEmpty())
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                 "open_rom_bytes called with no filename and no preloaded bytes");
        }
        Result<std::vector<std::uint8_t>> rom_data =
            read_rom(filename.toStdString(), file_repository_);
        if (!rom_data.has_value())
        {
            return std::unexpected(rom_data.error());
        }
        ecu_cal_def.FullRomData =
            bytes::toQByteArray(bytes::ByteView(rom_data->data(), rom_data->size()));
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

const std::vector<config::ResolvedCarModel> *LegacyCalibrationAdapter::resolved_car_models(
    const config::ConfigPaths& paths)
{
    if (resolved_car_models_cache_.has_value() &&
        resolved_car_models_handle_ == paths.protocols_file)
    {
        return &*resolved_car_models_cache_;
    }

    Result<config::ProtocolCatalog> protocols =
        config::load_protocol_catalog(paths, file_repository_);
    Result<config::CarModelCatalog> car_models =
        config::load_car_model_catalog(paths, file_repository_);
    if (!protocols.has_value() || !car_models.has_value())
    {
        // Not cached: a transient failure (file not yet provisioned) must not
        // pin an empty result for the rest of the process's life.
        return nullptr;
    }

    resolved_car_models_cache_ = config::resolve_car_models(*protocols, *car_models);
    resolved_car_models_handle_ = paths.protocols_file;
    return &*resolved_car_models_cache_;
}

void LegacyCalibrationAdapter::bind_protocol(
    definitions::ConfigValuesStructure& config_values, const QString& flash_method)
{
    const std::vector<config::ResolvedCarModel> *resolved =
        resolved_car_models(paths_from_config_values(config_values));
    if (resolved == nullptr)
    {
        return;
    }

    const std::optional<std::size_t> index =
        config::find_car_model_by_protocol_name(*resolved, flash_method.toStdString());
    if (!index.has_value())
    {
        return;
    }

    const config::ResolvedCarModel& row = (*resolved)[*index];

    // Legacy's placeholder for a protocol-derived field belonging to a car
    // model whose protocol_name matched no <protocol>: a single space, not
    // an empty string. Kept identical to legacy_config_adapter.cpp's own
    // kPlaceholder, which is what actually filled the parallel
    // flash_protocol_* QStringLists that open_subaru_rom_file's now-deleted
    // scan loop read these four values out of.
    const QString kPlaceholder(" ");

    // Every one of the nine is assigned unconditionally from here on. The
    // four protocol-derived ones fall back to the placeholder rather than
    // being skipped, so a previously bound ROM's values can never leak
    // through -- see bind_protocol's contract in the header.
    const auto protocol_field = [&row, &kPlaceholder](std::string config::ProtocolEntry::*field)
    {
        return row.protocol.has_value() ? QString::fromStdString((*row.protocol).*field)
                                        : kPlaceholder;
    };

    config_values.flash_protocol_selected_id = QString::number(*index);
    config_values.flash_protocol_selected_make = QString::fromStdString(row.make);
    config_values.flash_protocol_selected_model = QString::fromStdString(row.model);
    config_values.flash_protocol_selected_version = QString::fromStdString(row.version);
    config_values.flash_protocol_selected_protocol_name =
        QString::fromStdString(row.protocol_name);
    config_values.flash_protocol_selected_description =
        protocol_field(&config::ProtocolEntry::description);
    config_values.flash_protocol_selected_log_protocol =
        protocol_field(&config::ProtocolEntry::log_protocol);
    config_values.flash_protocol_selected_mcu = protocol_field(&config::ProtocolEntry::mcu);
    config_values.flash_protocol_selected_checksum =
        protocol_field(&config::ProtocolEntry::checksum);
}

void LegacyCalibrationAdapter::apply_flash_method_padding(
    definitions::EcuCalDefStructure& ecu_cal_def, const QString& flash_method)
{
    std::vector<std::uint8_t> rom_data(
        ecu_cal_def.FullRomData.cbegin(), ecu_cal_def.FullRomData.cend());
    rom_data = fastecu::calibration::apply_flash_method_padding(
        std::move(rom_data), flash_method.toStdString());
    ecu_cal_def.FullRomData = bytes::toQByteArray(bytes::ByteView(rom_data));
}

Status LegacyCalibrationAdapter::compute_map_cell_values(
    definitions::EcuCalDefStructure& ecu_cal_def,
    const definition::RomDefinition& rom_definition)
{
    auto computed = fastecu::calibration::compute_map_cell_values(
        rom_definition, bytes::view(ecu_cal_def.FullRomData), kFloatPrecision);
    if (!computed.has_value())
    {
        return std::unexpected(computed.error());
    }
    if (static_cast<qsizetype>(computed->size()) != ecu_cal_def.MapData.size())
    {
        return fail(ErrorKind::Internal,
                    std::format("definition has {} maps but legacy columns hold {}",
                                computed->size(),
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

definitions::EcuCalDefStructure *LegacyCalibrationAdapter::save_subaru_rom_file(
    definitions::EcuCalDefStructure *ecu_cal_def, const QString& filename)
{
    // Straight to the repository: there is no save-side policy for a
    // calibration_service function to carry, unlike the open path's
    // fire-and-forget backup_rom.
    const Status result =
        file_repository_.write(filename.toStdString(), bytes::view(ecu_cal_def->FullRomData));
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
