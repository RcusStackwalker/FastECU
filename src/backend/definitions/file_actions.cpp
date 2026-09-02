#include "src/backend/definitions/file_actions.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/algorithms/diagnostics/nrc_parser.h"
#include "src/backend/logging/legacy_logger_adapter.h"
#include "src/backend/logging/logger_conf.h"
#include "src/backend/logging/logger_definition_service.h"

namespace
{

using fastecu::definition::DefinitionCatalog;
using fastecu::definition::DefinitionFormat;
using fastecu::definition::DefinitionIndexEntry;
using fastecu::definition::IdEncoding;

fastecu::Result<DefinitionCatalog> catalogFromLegacyLists(const FileActions::ConfigValuesStructure& config,
                                                          DefinitionFormat format)
{
    const QStringList *ids =
        format == DefinitionFormat::RomRaider ? &config.romraider_def_cal_id : &config.ecuflash_def_cal_id;
    const QStringList *addresses =
        format == DefinitionFormat::RomRaider ? &config.romraider_def_cal_id_addr : &config.ecuflash_def_cal_id_addr;
    const QStringList *ecuIds =
        format == DefinitionFormat::RomRaider ? &config.romraider_def_ecu_id : &config.ecuflash_def_ecu_id;
    const QStringList *sources =
        format == DefinitionFormat::RomRaider ? &config.romraider_def_filename : &config.ecuflash_def_filename;

    const bool completeShape =
        sources->size() == ids->size() && addresses->size() == ids->size() && ecuIds->size() == ids->size();
    const bool readOnlyShape = sources->size() == ids->size() && addresses->isEmpty() && ecuIds->isEmpty();
    if (!completeShape && !readOnlyShape)
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "legacy definition catalog ID/source/address/ECU columns are not "
                             "aligned; expected four complete columns or the documented "
                             "ID/source-only read shape");
    }

    std::vector<DefinitionIndexEntry> entries;
    entries.reserve(static_cast<std::size_t>(ids->size()));
    for (qsizetype index = 0; index < ids->size(); ++index)
    {
        std::optional<std::uint64_t> address;
        const QString addressText = readOnlyShape ? QString{} : addresses->at(index);
        if (!addressText.trimmed().isEmpty())
        {
            bool valid = false;
            const qulonglong parsed = addressText.toULongLong(&valid, 16);
            if (!valid)
            {
                return fastecu::fail(
                    fastecu::ErrorKind::InvalidConfig,
                    std::format("invalid hexadecimal internal ID address '{}'", addressText.toStdString()));
            }
            address = static_cast<std::uint64_t>(parsed);
        }

        entries.push_back(DefinitionIndexEntry{
            .format = format,
            .definition_id = ids->at(index).toStdString(),
            .internal_id = ids->at(index).toStdString(),
            .internal_id_address = address,
            .internal_id_encoding = IdEncoding::AsciiOrHex,
            .ecu_id = readOnlyShape ? std::string{} : ecuIds->at(index).toStdString(),
            .source = sources->at(index).toStdString(),
        });
    }
    return DefinitionCatalog::create(std::move(entries));
}

void validateListLength(const QString& group, const QString& name, int actual, int expected, QStringList *errors)
{
    if (actual == expected)
    {
        return;
    }
    errors->append(QString("%1.%2 has %3 entries, expected %4").arg(group, name).arg(actual).arg(expected));
}

bool validateRequiredField(const QString& group, const QString& name, const QStringList& values, QStringList *errors)
{
    bool valid = true;
    for (int i = 0; i < values.size(); ++i)
    {
        const QString value = values.at(i).trimmed();
        if (value.isEmpty() || value.startsWith("No "))
        {
            errors->append(QString("%1.%2[%3] is required").arg(group, name).arg(i));
            valid = false;
        }
    }
    return valid;
}

void logValidationErrors(const QString& group, const QStringList& errors)
{
    for (const QString& error : errors)
    {
        qWarning().noquote() << group << error;
    }
}

std::vector<std::string> toStdStrings(const QStringList& values)
{
    return values | std::views::transform([](const QString& value) { return value.toStdString(); }) |
           std::ranges::to<std::vector>();
}

int lineAfterClosingTag(const QStringList& lines, const QString& tagName)
{
    const QString closeTag = "</" + tagName + ">";
    for (int i = 0; i < lines.size(); ++i)
    {
        if (lines.at(i).contains(closeTag))
        {
            return i + 1;
        }
    }
    return -1;
}

} // namespace

FileActions::FileActions(fastecu::IFileSystem& file_system, fastecu::IResourceBundle& resource_bundle,
                         fastecu::IFileRepository& file_repository, fastecu::IAtomicFileWriter& atomic_file_writer,
                         fastecu::IEventSink& events)
    : configAdapter_(file_system, resource_bundle, file_repository), definitionFileSystem_(file_system),
      definitionFileRepository_(file_repository), loggerResourceBundle_(resource_bundle),
      loggerAtomicFileWriter_(atomic_file_writer), definitionService_(file_system, file_repository, atomic_file_writer),
      definitionAdapter_(definitionService_), calibrationAdapter_(file_repository), events_(events)
{
}

fastecu::Result<DefinitionCatalog> FileActions::build_definition_catalog(DefinitionFormat format)
{
    if (format == DefinitionFormat::RomRaider && !ConfigValuesStruct.romraider_definition_files.isEmpty())
    {
        std::vector<std::string> handles;
        handles.reserve(static_cast<std::size_t>(ConfigValuesStruct.romraider_definition_files.size()));
        for (const QString& handle : ConfigValuesStruct.romraider_definition_files)
        {
            handles.push_back(handle.toStdString());
        }
        return definitionService_.build_romraider_catalog(handles);
    }
    if (format == DefinitionFormat::EcuFlash)
    {
        std::vector<std::string> explicitHandles;
        explicitHandles.reserve(static_cast<std::size_t>(ConfigValuesStruct.ecuflash_def_filename.size()));
        for (const QString& handle : ConfigValuesStruct.ecuflash_def_filename)
        {
            explicitHandles.push_back(handle.toStdString());
        }
        if (ConfigValuesStruct.ecuflash_definition_files_directory.isEmpty() && explicitHandles.empty())
        {
            return catalogFromLegacyLists(ConfigValuesStruct, format);
        }
        return definitionService_.build_ecuflash_catalog(
            ConfigValuesStruct.ecuflash_definition_files_directory.toStdString(), explicitHandles);
    }
    return catalogFromLegacyLists(ConfigValuesStruct, format);
}

QString FileActions::definition_source(DefinitionFormat format, const QString& id) const
{
    const QStringList *ids = format == DefinitionFormat::RomRaider ? &ConfigValuesStruct.romraider_def_cal_id
                                                                   : &ConfigValuesStruct.ecuflash_def_cal_id;
    const QStringList *sources = format == DefinitionFormat::RomRaider ? &ConfigValuesStruct.romraider_def_filename
                                                                       : &ConfigValuesStruct.ecuflash_def_filename;
    const qsizetype index = ids->indexOf(id);
    return index >= 0 && index < sources->size() ? sources->at(index) : QString{};
}

void FileActions::log_definition_error(const QString& operation, const fastecu::Error& error)
{
    events_.log(fastecu::LogLevel::Error,
                std::format("{} [{}]: {}", operation.toStdString(), fastecu::to_string(error.kind), error.detail));
}

fastecu::Status FileActions::load_configured_definition(EcuCalDefStructure& ecu_cal_def, DefinitionFormat format,
                                                        const QString& definition_id)
{
    auto catalog = build_definition_catalog(format);
    if (!catalog.has_value())
    {
        resolvedDefinition_.reset();
        return std::unexpected(catalog.error());
    }
    fastecu::definition::RomDefinition resolved;
    fastecu::Status replaced =
        definitionAdapter_.replace_definition(ecu_cal_def, *catalog, format, definition_id.toStdString(), &resolved);
    if (!replaced.has_value())
    {
        resolvedDefinition_.reset();
        return replaced;
    }
    // Keep the resolution this call already paid for: open_subaru_rom_file's
    // ROM-size validation needs the same RomDefinition, and re-deriving it
    // there would mean a second full definition-catalog build (which
    // index-parses every XML in the definitions directory) per ROM open.
    resolvedDefinition_ = ResolvedDefinition{
        .format = format,
        .id = definition_id,
        .definition = std::move(resolved),
    };
    normalize_definition_addresses(ecu_cal_def);
    apply_flash_method_alias(ecu_cal_def);
    return {};
}

const fastecu::definition::RomDefinition *FileActions::resolved_definition(DefinitionFormat format,
                                                                           const QString& definition_id) const
{
    if (!resolvedDefinition_.has_value() || resolvedDefinition_->format != format ||
        resolvedDefinition_->id != definition_id)
    {
        return nullptr;
    }
    return &resolvedDefinition_->definition;
}

bool FileActions::log_definition_load_failure(const QString& operation, const fastecu::Error& error,
                                              const QString& source, const QString& warning_title,
                                              const QString& warning_text)
{
    log_definition_error(operation, error);
    if (!source.isEmpty() && !definitionFileSystem_.exists(source.toStdString()))
    {
        events_.notice(std::format("{}: {}{} for reading", warning_title.toStdString(), warning_text.toStdString(),
                                   source.toStdString()));
        return true;
    }
    return false;
}

void FileActions::strip_legacy_address_prefixes(QStringList& addresses)
{
    for (QString& address : addresses)
    {
        if (address.startsWith("0x"))
        {
            address.remove(0, 2);
        }
    }
}

fastecu::Status FileActions::submit_new_definition(std::string_view destination,
                                                   const fastecu::definition::DefinitionHeaderInput& input)
{
    // The caller reaches this only after the native "Save As" dialog already asked to
    // overwrite an existing file, so that confirmation is passed through here rather than
    // rejected again by the service's own new-file-only guard.
    fastecu::Status status = definitionAdapter_.create_definition(destination, input, /*allow_overwrite=*/true);
    if (!status.has_value())
    {
        log_definition_error("Unable to create definition", status.error());
    }
    else
    {
        remember_submitted_ecuflash_handle(destination);
    }
    return status;
}

fastecu::Status FileActions::submit_imported_definition(std::string_view source, std::string_view destination,
                                                        const fastecu::definition::DefinitionHeaderInput& input)
{
    fastecu::Status status = definitionAdapter_.import_definition(source, destination, input);
    if (!status.has_value())
    {
        log_definition_error("Unable to import definition", status.error());
    }
    else
    {
        remember_submitted_ecuflash_handle(destination);
    }
    return status;
}

void FileActions::remember_submitted_ecuflash_handle(std::string_view destination)
{
    const auto position = std::ranges::lower_bound(submittedEcuflashHandles_, destination, {},
                                                   [](const auto& item) { return std::string_view{item}; });
    if (position == std::ranges::end(submittedEcuflashHandles_) || std::string_view(*position) != destination)
    {
        submittedEcuflashHandles_.emplace(position, destination);
    }
}

void FileActions::apply_flash_method_alias(EcuCalDefStructure& ecuCalDef)
{
    if (ecuCalDef.RomInfo.size() <= FlashMethod)
    {
        return;
    }
    const QString flashMethod = ecuCalDef.RomInfo.at(FlashMethod);
    for (qsizetype index = 0; index < ConfigValuesStruct.flash_protocol_id.size() &&
                              index < ConfigValuesStruct.flash_protocol_alias.size() &&
                              index < ConfigValuesStruct.flash_protocol_protocol_name.size();
         ++index)
    {
        const QStringList aliases = ConfigValuesStruct.flash_protocol_alias.at(index).split(",");
        if (aliases.contains(flashMethod))
        {
            events_.log(fastecu::LogLevel::Debug, std::format("Alias: {}", flashMethod.toStdString()));
            events_.log(
                fastecu::LogLevel::Debug,
                std::format("Protocol: {}", ConfigValuesStruct.flash_protocol_protocol_name.at(index).toStdString()));
            ecuCalDef.RomInfo.replace(FlashMethod, ConfigValuesStruct.flash_protocol_protocol_name.at(index));
            return;
        }
    }
}

void FileActions::normalize_definition_addresses(EcuCalDefStructure& ecuCalDef)
{
    const auto removePrefix = [](QString& address)
    {
        if (address.startsWith("0x"))
        {
            address.remove(0, 2);
        }
    };
    if (ecuCalDef.RomInfo.size() > InternalIdAddress)
    {
        removePrefix(ecuCalDef.RomInfo[InternalIdAddress]);
    }
    for (QStringList *addresses : {&ecuCalDef.AddressList, &ecuCalDef.XScaleAddressList, &ecuCalDef.YScaleAddressList})
    {
        for (QString& address : *addresses)
        {
            removePrefix(address);
        }
    }
}

bool FileActions::validate_flash_protocols(const ConfigValuesStructure& configValues, QStringList *errors)
{
    QStringList localErrors;
    QStringList *out = errors ? errors : &localErrors;
    const int rows = configValues.flash_protocol_id.size();

    validateListLength("flash_protocol", "alias", configValues.flash_protocol_alias.size(), rows, out);
    validateListLength("flash_protocol", "make", configValues.flash_protocol_make.size(), rows, out);
    validateListLength("flash_protocol", "model", configValues.flash_protocol_model.size(), rows, out);
    validateListLength("flash_protocol", "version", configValues.flash_protocol_version.size(), rows, out);
    validateListLength("flash_protocol", "type", configValues.flash_protocol_type.size(), rows, out);
    validateListLength("flash_protocol", "kw", configValues.flash_protocol_kw.size(), rows, out);
    validateListLength("flash_protocol", "hp", configValues.flash_protocol_hp.size(), rows, out);
    validateListLength("flash_protocol", "fuel", configValues.flash_protocol_fuel.size(), rows, out);
    validateListLength("flash_protocol", "year", configValues.flash_protocol_year.size(), rows, out);
    validateListLength("flash_protocol", "ecu", configValues.flash_protocol_ecu.size(), rows, out);
    validateListLength("flash_protocol", "mcu", configValues.flash_protocol_mcu.size(), rows, out);
    validateListLength("flash_protocol", "mode", configValues.flash_protocol_mode.size(), rows, out);
    validateListLength("flash_protocol", "checksum", configValues.flash_protocol_checksum.size(), rows, out);
    validateListLength("flash_protocol", "read", configValues.flash_protocol_read.size(), rows, out);
    validateListLength("flash_protocol", "test_write", configValues.flash_protocol_test_write.size(), rows, out);
    validateListLength("flash_protocol", "write", configValues.flash_protocol_write.size(), rows, out);
    validateListLength("flash_protocol", "flash_transport", configValues.flash_protocol_flash_transport.size(), rows,
                       out);
    validateListLength("flash_protocol", "log_transport", configValues.flash_protocol_log_transport.size(), rows, out);
    validateListLength("flash_protocol", "log_protocol", configValues.flash_protocol_log_protocol.size(), rows, out);
    validateListLength("flash_protocol", "ecu_id_ascii", configValues.flash_protocol_ecu_id_ascii.size(), rows, out);
    validateListLength("flash_protocol", "ecu_id_addr", configValues.flash_protocol_ecu_id_addr.size(), rows, out);
    validateListLength("flash_protocol", "ecu_id_length", configValues.flash_protocol_ecu_id_length.size(), rows, out);
    validateListLength("flash_protocol", "cal_id_ascii", configValues.flash_protocol_cal_id_ascii.size(), rows, out);
    validateListLength("flash_protocol", "cal_id_addr", configValues.flash_protocol_cal_id_addr.size(), rows, out);
    validateListLength("flash_protocol", "cal_id_length", configValues.flash_protocol_cal_id_length.size(), rows, out);
    validateListLength("flash_protocol", "kernel", configValues.flash_protocol_kernel.size(), rows, out);
    validateListLength("flash_protocol", "kernel_addr", configValues.flash_protocol_kernel_addr.size(), rows, out);
    validateListLength("flash_protocol", "description", configValues.flash_protocol_description.size(), rows, out);
    validateListLength("flash_protocol", "protocol_name", configValues.flash_protocol_protocol_name.size(), rows, out);

    validateRequiredField("flash_protocol", "id", configValues.flash_protocol_id, out);
    validateRequiredField("flash_protocol", "protocol_name", configValues.flash_protocol_protocol_name, out);

    return out->isEmpty();
}

bool FileActions::validate_logger_values(const LogValuesStructure& logValues, QStringList *errors)
{
    QStringList localErrors;
    QStringList *out = errors ? errors : &localErrors;
    const int rows = logValues.log_value_id.size();

    validateListLength("log_value", "protocol", logValues.log_value_protocol.size(), rows, out);
    validateListLength("log_value", "name", logValues.log_value_name.size(), rows, out);
    validateListLength("log_value", "description", logValues.log_value_description.size(), rows, out);
    validateListLength("log_value", "ecu_byte_index", logValues.log_value_ecu_byte_index.size(), rows, out);
    validateListLength("log_value", "ecu_bit", logValues.log_value_ecu_bit.size(), rows, out);
    validateListLength("log_value", "target", logValues.log_value_target.size(), rows, out);
    validateListLength("log_value", "address", logValues.log_value_address.size(), rows, out);
    validateListLength("log_value", "conversions", logValues.log_value_conversions.size(), rows, out);
    validateListLength("log_value", "length", logValues.log_value_length.size(), rows, out);
    validateListLength("log_value", "value", logValues.log_value.size(), rows, out);
    validateListLength("log_value", "enabled", logValues.log_value_enabled.size(), rows, out);

    validateRequiredField("log_value", "protocol", logValues.log_value_protocol, out);
    validateRequiredField("log_value", "id", logValues.log_value_id, out);

    return out->isEmpty();
}

bool FileActions::validate_logger_switches(const LogValuesStructure& logValues, QStringList *errors)
{
    QStringList localErrors;
    QStringList *out = errors ? errors : &localErrors;
    const int rows = logValues.log_switch_id.size();

    validateListLength("log_switch", "protocol", logValues.log_switch_protocol.size(), rows, out);
    validateListLength("log_switch", "name", logValues.log_switch_name.size(), rows, out);
    validateListLength("log_switch", "description", logValues.log_switch_description.size(), rows, out);
    validateListLength("log_switch", "address", logValues.log_switch_address.size(), rows, out);
    validateListLength("log_switch", "ecu_byte_index", logValues.log_switch_ecu_byte_index.size(), rows, out);
    validateListLength("log_switch", "ecu_bit", logValues.log_switch_ecu_bit.size(), rows, out);
    validateListLength("log_switch", "target", logValues.log_switch_target.size(), rows, out);
    validateListLength("log_switch", "enabled", logValues.log_switch_enabled.size(), rows, out);
    validateListLength("log_switch", "state", logValues.log_switch_state.size(), rows, out);

    validateRequiredField("log_switch", "protocol", logValues.log_switch_protocol, out);
    validateRequiredField("log_switch", "id", logValues.log_switch_id, out);

    return out->isEmpty();
}

bool FileActions::validate_calibration_maps(const EcuCalDefStructure& ecuCalDef, QStringList *errors)
{
    QStringList localErrors;
    QStringList *out = errors ? errors : &localErrors;
    const int rows = ecuCalDef.IdList.size();

    validateListLength("calibration_map", "type", ecuCalDef.TypeList.size(), rows, out);
    validateListLength("calibration_map", "name", ecuCalDef.NameList.size(), rows, out);
    validateListLength("calibration_map", "address", ecuCalDef.AddressList.size(), rows, out);
    validateListLength("calibration_map", "category", ecuCalDef.CategoryList.size(), rows, out);
    validateListLength("calibration_map", "x_size", ecuCalDef.XSizeList.size(), rows, out);
    validateListLength("calibration_map", "y_size", ecuCalDef.YSizeList.size(), rows, out);
    validateListLength("calibration_map", "format", ecuCalDef.FormatList.size(), rows, out);
    validateListLength("calibration_map", "units", ecuCalDef.UnitsList.size(), rows, out);
    validateListLength("calibration_map", "storage_type", ecuCalDef.StorageTypeList.size(), rows, out);
    validateListLength("calibration_map", "endian", ecuCalDef.EndianList.size(), rows, out);
    validateListLength("calibration_map", "from_byte", ecuCalDef.FromByteList.size(), rows, out);
    validateListLength("calibration_map", "to_byte", ecuCalDef.ToByteList.size(), rows, out);
    validateListLength("calibration_map", "defined", ecuCalDef.MapDefined.size(), rows, out);
    validateListLength("calibration_map", "subcategory", ecuCalDef.SubCategoryList.size(), rows, out);
    validateListLength("calibration_map", "level", ecuCalDef.LevelList.size(), rows, out);
    validateListLength("calibration_map", "user_level", ecuCalDef.UserLevelList.size(), rows, out);
    validateListLength("calibration_map", "swap_xy", ecuCalDef.SwapXYList.size(), rows, out);
    validateListLength("calibration_map", "flip_x", ecuCalDef.FlipXList.size(), rows, out);
    validateListLength("calibration_map", "flip_y", ecuCalDef.FlipYList.size(), rows, out);

    validateRequiredField("calibration_map", "id", ecuCalDef.IdList, out);
    validateRequiredField("calibration_map", "name", ecuCalDef.NameList, out);

    return out->isEmpty();
}

QStringList FileActions::collect_ecuflash_base_header_fields(const EcuCalDefStructure& ecuCalDef,
                                                             const QStringList& defData, int *endIndex)
{
    QStringList headerData;
    QHash<QString, QString> values;

    QDomDocument xml;
    if (xml.setContent(defData.join(QString())))
    {
        QDomElement root = xml.documentElement();
        int depth = 0;
        while (!root.isNull() && root.tagName() != "rom" && depth < 5)
        {
            root = root.firstChildElement();
            ++depth;
        }

        const QDomElement romid = root.firstChildElement("romid");
        for (const QString& name : ecuCalDef.DefHeaderNames)
        {
            const QDomElement element =
                (name == "include" || name == "notes") ? root.firstChildElement(name) : romid.firstChildElement(name);
            if (!element.isNull())
            {
                values.insert(name, element.text());
            }
        }
    }

    for (const QString& name : ecuCalDef.DefHeaderNames)
    {
        headerData << name << values.value(name);
    }

    if (endIndex)
    {
        int nextLine = lineAfterClosingTag(defData, "notes");
        if (nextLine < 0)
        {
            nextLine = lineAfterClosingTag(defData, "include");
        }
        if (nextLine < 0)
        {
            nextLine = lineAfterClosingTag(defData, "romid");
        }
        *endIndex = qMax(0, nextLine);
    }

    return headerData;
}

QStringList FileActions::collect_ecuflash_definition_body_lines(const QStringList& defData, int startIndex)
{
    QStringList bodyLines;
    for (int i = qMax(0, startIndex); i < defData.size(); ++i)
    {
        if (defData.at(i).trimmed() == "</rom>")
        {
            continue;
        }
        bodyLines.append(defData.at(i));
    }
    return bodyLines;
}

FileActions::ConfigValuesStructure *FileActions::set_base_dirs(ConfigValuesStructure *configValues,
                                                               std::string_view app_root_path)
{
    return configAdapter_.set_base_dirs(configValues, app_root_path);
}

FileActions::ConfigValuesStructure *FileActions::check_config_dirs(ConfigValuesStructure *configValues)
{
    return configAdapter_.check_config_dirs(configValues);
}

FileActions::ConfigValuesStructure *FileActions::read_config_file(ConfigValuesStructure *configValues)
{
    return configAdapter_.read_config_file(configValues);
}

FileActions::ConfigValuesStructure *FileActions::save_config_file(FileActions::ConfigValuesStructure *configValues)
{
    return configAdapter_.save_config_file(configValues);
}

FileActions::ConfigValuesStructure *FileActions::read_protocols_file(FileActions::ConfigValuesStructure *configValues)
{
    configAdapter_.read_protocols_file(configValues);

    // Restores legacy read_protocols_file's final step (file_actions.cpp
    // history, formerly lines ~1385-1389): validate the populated
    // flash_protocol_* lists and log (not surface as an error) whatever
    // validate_flash_protocols finds. This runs here rather than inside
    // LegacyConfigAdapter::read_protocols_file because validate_flash_protocols
    // is a FileActions static method, and //src/backend/definitions
    // (FileActions's target) already depends on
    // //src/backend/config:legacy_config_adapter -- the adapter calling
    // back into FileActions would form a hard Bazel dependency cycle (see
    // src/backend/config/BUILD.bazel's legacy_config_adapter comment for
    // the same constraint already documented there). FileActions is the
    // only public entry point every caller (MainWindow, protocol_select,
    // vehicle_select) actually uses, so validation still runs on every
    // real read_protocols_file call.
    QStringList validationErrors;
    if (!validate_flash_protocols(*configValues, &validationErrors))
    {
        logValidationErrors("Invalid protocols file:", validationErrors);
    }

    return configValues;
}

FileActions::LogValuesStructure *FileActions::read_logger_conf(FileActions::LogValuesStructure *logValues,
                                                               const QString& ecu_id, bool modify)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    const std::string handle = configValues->logger_file.toStdString();
    const std::string ecu_key = ecu_id.toStdString();

    events_.log(fastecu::LogLevel::Debug, std::format("Looking for ECU ID: {} in logger def file: {}",
                                                      ecu_id.toStdString(), configValues->logger_file.toStdString()));

    const auto warnUnreadable = [&]
    {
        events_.notice(std::format("Logger file: Unable to open logger config file '{}' for reading",
                                   configValues->logger_file.toStdString()));
    };

    fastecu::logging::LoggerDefinitionService service(definitionFileRepository_, loggerResourceBundle_,
                                                      loggerAtomicFileWriter_);

    if (modify)
    {
        fastecu::logging::LoggerSelection selection{.protocol = logValues->logging_values_protocol.toStdString(),
                                                    .gauge_ids = toStdStrings(logValues->dashboard_log_value_id),
                                                    .lower_panel_ids =
                                                        toStdStrings(logValues->lower_panel_log_value_id),
                                                    .switch_ids = toStdStrings(logValues->lower_panel_switch_id)};
        if (auto saved = service.save_selection(handle, ecu_key, selection); !saved)
        {
            warnUnreadable();
            return nullptr;
        }
        return logValues;
    }

    // A read must never write: load_selection reports "this ECU has no entry"
    // as an empty optional and leaves the file alone, so the no-definition
    // check below still runs before anything is persisted.
    const auto stored = service.load_selection(handle, ecu_key);
    if (!stored)
    {
        warnUnreadable();
        return nullptr;
    }

    // Legacy cleared the three selection lists here -- right after the conf
    // file opened and before it knew whether this ECU had an entry. Preserved
    // deliberately: on the no-definition-loaded branch below this method
    // returns without repopulating them, and MainWindow::update_logbox_values
    // walks lower_panel_log_value_id against the log box layout that
    // update_logboxes just rebuilt from an empty definition. Leaving stale ids
    // in the list there would index past the end of an empty layout.
    logValues->dashboard_log_value_id.clear();
    logValues->lower_panel_log_value_id.clear();
    logValues->lower_panel_switch_id.clear();

    if (stored->has_value())
    {
        events_.log(fastecu::LogLevel::Debug, std::format("Found ECU ID {}", ecu_id.toStdString()));
        fastecu::logging::apply_selection(**stored, *logValues);
        return logValues;
    }

    // Only reachable with the ECU id absent, which is the only state legacy
    // ever surfaced this warning from.
    if (logValues->log_value_protocol.empty())
    {
        events_.notice("Logger definition file: No logger definition file selected, returning without initializing log "
                       "parameters!");
        events_.log(fastecu::LogLevel::Debug,
                    "No logger definition file selected, returning without initializing log parameters!");
        return nullptr;
    }

    events_.log(fastecu::LogLevel::Debug, "ECU ID not found, initializing log parameters");

    // Only the three fields default_selection reads. zip truncates to the
    // shortest list, so a caller-supplied struct whose parallel arrays are
    // skewed yields fewer rows instead of indexing past the end of one.
    fastecu::logging::LoggerDefinition definition;
    for (const auto& [protocol, id, enabled] :
         std::views::zip(logValues->log_value_protocol, logValues->log_value_id, logValues->log_value_enabled))
    {
        definition.parameters.push_back(
            {.protocol = protocol.toStdString(), .id = id.toStdString(), .enabled = enabled == "1"});
    }
    // No protocol here: default_selection reads LoggerSwitch::protocol
    // nowhere, and zipping log_switch_protocol in would truncate the walk
    // against a list legacy never consulted on this path.
    for (const auto& [id, enabled] : std::views::zip(logValues->log_switch_id, logValues->log_switch_enabled))
    {
        // `enabled` carries the ECU's runtime capability response, not the XML
        // default -- this is the state default_selection must filter on.
        definition.switches.push_back({.id = id.toStdString(), .enabled = enabled == "1"});
    }

    const auto selection = service.load_or_initialize_selection(handle, ecu_key, definition);
    if (!selection)
    {
        warnUnreadable();
        return nullptr;
    }
    fastecu::logging::apply_selection(*selection, *logValues);

    return logValues;
}

FileActions::LogValuesStructure *FileActions::read_logger_definition_file()
{
    LogValuesStructure *logValues = &LogValuesStruct;
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    fastecu::logging::LoggerDefinitionService service(definitionFileRepository_, loggerResourceBundle_,
                                                      loggerAtomicFileWriter_);

    const auto handle =
        service.resolve_definition_handle(configValues->romraider_logger_definition_file.toStdString(),
                                          configValues->flash_protocol_selected_log_protocol.toStdString(),
                                          configValues->config_files_directory.toStdString());
    if (!handle)
    {
        events_.notice(std::format("Logger file: Unable to resolve logger definition file: {}", handle.error().detail));
        return logValues;
    }
    if (configValues->romraider_logger_definition_file.isEmpty() && !handle->empty())
    {
        configValues->romraider_logger_definition_file = QString::fromStdString(*handle);
        events_.log(fastecu::LogLevel::Debug, std::format("Using bundled CDBG logger definition: {}", *handle));
    }

    const auto definition = service.load_definition(*handle);
    if (!definition)
    {
        events_.notice(std::format("Logger file: Unable to open logger definition file '{}' for reading: {}", *handle,
                                   definition.error().detail));
        return logValues;
    }

    fastecu::logging::apply_definition(*definition, *logValues);
    fastecu::logging::apply_selection(fastecu::logging::initial_selection(*definition), *logValues);

    QStringList validationErrors;
    validate_logger_values(*logValues, &validationErrors);
    validate_logger_switches(*logValues, &validationErrors);
    if (!validationErrors.isEmpty())
    {
        logValidationErrors("Invalid logger definition:", validationErrors);
    }

    return logValues;
}

QString FileActions::parse_hex_ecuid(uint8_t byte)
{
    QString ecuid_byte;
    static constexpr std::string_view chars = "0123456789ABCDEF";

    ecuid_byte = (QChar)chars[(byte >> 4) & 0xF];
    ecuid_byte.append((QChar)chars[(byte >> 0) & 0xF]);
    // emit LOG_D("Constructed byte: " + ecuid_byte;

    return ecuid_byte;
}

FileActions::EcuCalDefStructure *FileActions::parse_ecuid_ecuflash_def_files(FileActions::EcuCalDefStructure *ecuCalDef,
                                                                             bool is_ascii)
{
    (void)is_ascii;
    auto catalog = build_definition_catalog(DefinitionFormat::EcuFlash);
    if (!catalog.has_value())
    {
        log_definition_error("Unable to match EcuFlash definition", catalog.error());
        return ecuCalDef;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(ecuCalDef->FullRomData.constData());
    auto match = definitionService_.match_rom(
        *catalog, std::span<const std::uint8_t>(bytes, static_cast<std::size_t>(ecuCalDef->FullRomData.size())));
    if (!match.has_value())
    {
        log_definition_error("Unable to match EcuFlash definition", match.error());
        return ecuCalDef;
    }
    ecuCalDef->RomId = QString::fromStdString(match->definition_id);
    events_.log(fastecu::LogLevel::Debug, std::format("EcuFlash cal id {} found", match->definition_id));
    return ecuCalDef;
}

FileActions::EcuCalDefStructure *
FileActions::parse_ecuid_romraider_def_files(FileActions::EcuCalDefStructure *ecuCalDef, bool is_ascii)
{
    (void)is_ascii;
    auto catalog = build_definition_catalog(DefinitionFormat::RomRaider);
    if (!catalog.has_value())
    {
        log_definition_error("Unable to match RomRaider definition", catalog.error());
        return ecuCalDef;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(ecuCalDef->FullRomData.constData());
    auto match = definitionService_.match_rom(
        *catalog, std::span<const std::uint8_t>(bytes, static_cast<std::size_t>(ecuCalDef->FullRomData.size())));
    if (!match.has_value())
    {
        log_definition_error("Unable to match RomRaider definition", match.error());
        return ecuCalDef;
    }
    ecuCalDef->RomId = QString::fromStdString(match->definition_id);
    events_.log(fastecu::LogLevel::Debug, std::format("RomRaider cal id {} found", match->definition_id));
    return ecuCalDef;
}

void FileActions::apply_missing_definition_defaults(FileActions::EcuCalDefStructure *ecuCalDef)
{
    if (ecuCalDef == nullptr || ecuCalDef->RomInfo.size() <= DefFile)
    {
        return;
    }
    ecuCalDef->RomInfo.replace(XmlId, "UnknownID");
    ecuCalDef->RomInfo.replace(InternalIdAddress, "");
    ecuCalDef->RomInfo.replace(InternalIdString, "");
    ecuCalDef->RomInfo.replace(EcuId, "");
    ecuCalDef->RomInfo.replace(Make, ConfigValuesStruct.flash_protocol_selected_make);
    // No FileSize write here: open_subaru_rom_file already set RomInfo[FileSize]
    // unconditionally from the pre-padding length, which is exactly the value
    // this block used to end up with. Rewriting it after the fact would pick up
    // the post-padding length instead and change what the user sees.
    ecuCalDef->RomInfo.replace(DefFile, " ");
}

FileActions::EcuCalDefStructure *FileActions::open_subaru_rom_file(FileActions::EcuCalDefStructure *ecuCalDef,
                                                                   QString filename)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    bool id_is_ascii = false;

    fastecu::Status opened = calibrationAdapter_.open_rom_bytes(*ecuCalDef, filename, *configValues);
    if (!opened.has_value())
    {
        log_definition_error("Unable to open calibration file", opened.error());
        events_.notice("Calibration file: Unable to open calibration file for reading");
        return nullptr;
    }
    filename = ecuCalDef->FullFileName;

    ecuCalDef->use_ecuflash_definition = false;
    ecuCalDef->use_romraider_definition = false;

    if ((configValues->primary_definition_base == "ecuflash" || configValues->use_romraider_definitions != "enabled") &&
        configValues->ecuflash_definition_files_directory.length())
    {
        if (configValues->use_ecuflash_definitions == "enabled")
        {
            parse_ecuid_ecuflash_def_files(ecuCalDef, id_is_ascii);
            if (ecuCalDef->RomId != "")
            {
                // emit LOG_D("Parse EcuFlash def files (primary) " + ecuCalDef->RomId;
                read_ecuflash_ecu_def(ecuCalDef, ecuCalDef->RomId);
            }
        }
        if (!ecuCalDef->use_ecuflash_definition && configValues->use_romraider_definitions == "enabled")
        {
            parse_ecuid_romraider_def_files(ecuCalDef, id_is_ascii);
            if (ecuCalDef->RomId != "")
            {
                // emit LOG_D("Parse RomRaider def files (secondary) " + ecuCalDef->RomId;
                read_romraider_ecu_def(ecuCalDef, ecuCalDef->RomId);
            }
        }
    }
    else if (configValues->primary_definition_base == "romraider" && !configValues->romraider_definition_files.empty())
    {
        if (configValues->use_romraider_definitions == "enabled")
        {
            parse_ecuid_romraider_def_files(ecuCalDef, id_is_ascii);
            if (ecuCalDef->RomId != "")
            {
                // emit LOG_D("Parse RomRaider def files (primary) " + ecuCalDef->RomId;
                read_romraider_ecu_def(ecuCalDef, ecuCalDef->RomId);
            }
        }
        if (!ecuCalDef->use_romraider_definition && configValues->use_ecuflash_definitions == "enabled")
        {
            parse_ecuid_ecuflash_def_files(ecuCalDef, id_is_ascii);
            if (ecuCalDef->RomId != "")
            {
                // emit LOG_D("Parse EcuFlash def files (secondary) " + ecuCalDef->RomId;
                read_ecuflash_ecu_def(ecuCalDef, ecuCalDef->RomId);
            }
        }
    }

    // The "no definition found" chooser dialog (create new / use existing /
    // continue without) lives in MainWindow, and so does the decision to
    // apply the continue-without placeholders: see
    // apply_missing_definition_defaults, which MainWindow calls only on the
    // continue-without / rejected path. Applying them here unconditionally
    // would stamp them over a definition the user just created or imported.

    calibrationAdapter_.bind_protocol(*configValues, ecuCalDef->RomInfo.at(FlashMethod));

    QString checksum_module = ecuCalDef->RomInfo.at(FlashMethod);
    checksum_module.remove(0, 3);
    checksum_module.insert(0, "checksum");
    if (configValues->flash_protocol_selected_checksum == "yes")
    {
        ecuCalDef->RomInfo.replace(ChecksumModule, checksum_module);
    }
    if (configValues->flash_protocol_selected_checksum == "n/a")
    {
        ecuCalDef->RomInfo.replace(ChecksumModule, "Not implemented yet");
    }
    if (configValues->flash_protocol_selected_checksum == "no")
    {
        ecuCalDef->RomInfo.replace(ChecksumModule, "No checksums");
    }

    // FileName/FullFileName were already set by
    // LegacyCalibrationAdapter::open_rom_bytes (including its "default.bin"
    // fallback for an empty basename).
    ecuCalDef->McuType = configValues->flash_protocol_selected_mcu;
    ecuCalDef->OemEcuFile = true;
    ecuCalDef->FileSize = QString::number(ecuCalDef->FullRomData.length());
    ecuCalDef->RomInfo.replace(FileSize, QString::number(ecuCalDef->FullRomData.length() / 1024) + "kb");

    // Must precede the ROM-size validation below: padding grows FullRomData by
    // 0x8000 bytes, and a definition authored against the padded image would be
    // rejected by a check run against the pre-padded length.
    calibrationAdapter_.apply_flash_method_padding(*ecuCalDef, ecuCalDef->RomInfo.at(FlashMethod));

    const fastecu::definition::DefinitionFormat matchedFormat = ecuCalDef->use_ecuflash_definition
                                                                    ? fastecu::definition::DefinitionFormat::EcuFlash
                                                                    : fastecu::definition::DefinitionFormat::RomRaider;
    const fastecu::definition::RomDefinition *romDefinition = nullptr;
    if (ecuCalDef->use_romraider_definition || ecuCalDef->use_ecuflash_definition)
    {
        romDefinition = resolved_definition(matchedFormat, ecuCalDef->RomId);
        if (romDefinition == nullptr)
        {
            // Both the size check and the map-value decode below need the
            // resolved definition, so both are skipped together here. Never
            // silent: the maps keep their default (empty) values, and the user
            // would otherwise see a ROM whose tables are blank for no stated
            // reason.
            events_.log(
                fastecu::LogLevel::Warning,
                std::format("ROM size validation and map value decoding skipped: no resolved definition for id {}",
                            ecuCalDef->RomId.toStdString()));
        }
        else
        {
            fastecu::Status sizeOk = fastecu::calibration::validate_rom_size(
                *romDefinition, static_cast<std::size_t>(ecuCalDef->FullRomData.length()));
            if (!sizeOk.has_value())
            {
                log_definition_error("Error in expected ROM size", sizeOk.error());
                events_.notice("File size error: Error in expected ROM size!");
                ecuCalDef->NameList.clear();
                return ecuCalDef;
            }
        }
    }

    if (romDefinition != nullptr)
    {
        const fastecu::Status computed = calibrationAdapter_.compute_map_cell_values(*ecuCalDef, *romDefinition);
        if (!computed.has_value())
        {
            log_definition_error("Error decoding calibration map values", computed.error());
        }
    }

    if (ecuCalDef == nullptr)
    {
        events_.notice("Calibration file: Unable to find definition for selected calibration file with ECU ID: .");
        return nullptr;
    }

    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::save_subaru_rom_file(FileActions::EcuCalDefStructure *ecuCalDef,
                                                                   const QString& filename)
{
    EcuCalDefStructure *saved = calibrationAdapter_.save_subaru_rom_file(ecuCalDef, filename);
    if (saved == nullptr)
    {
        // A failed write is the one failure in this file that must never be
        // silent: both callers in MainWindow historically ignored the return
        // value, so this notice is the user's only signal that the ROM they
        // are about to flash was not written.
        events_.log(fastecu::LogLevel::Error,
                    std::format("Unable to open file {} for writing", filename.toStdString()));
        events_.notice(std::format("Ecu calibration file: Unable to open file {} for writing", filename.toStdString()));
    }
    return saved;
}

QString FileActions::parse_nrc_message(const QByteArray& nrc)
{
    return QString::fromStdString(nrc_description(bytes::view(nrc)));
}

QString FileActions::parse_dtc_message(uint16_t dtc)
{
    return QString::fromStdString(dtc_description(dtc));
}
