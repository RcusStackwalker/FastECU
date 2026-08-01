// #include "src/backend/definitions/file_actions.h"
#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/backend/definitions/error_codes.h"
#include "src/algorithms/diagnostics/qt_dtc_parser.h"
#include "src/algorithms/expression/qt_expression_evaluator.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/checksum/flash_device_lookup.h"
#include "src/algorithms/diagnostics/qt_nrc_parser.h"

namespace
{

using fastecu::definition::DefinitionCatalog;
using fastecu::definition::DefinitionFormat;
using fastecu::definition::DefinitionIndexEntry;
using fastecu::definition::IdEncoding;

QString lineEditValue(
    const QList<QLineEdit *>& lineEdits,
    const QString& name)
{
    for (const QLineEdit *editor : lineEdits)
    {
        if (editor->objectName() == name)
        {
            return editor->text();
        }
    }
    return {};
}

fastecu::Result<fastecu::definition::DefinitionHeaderInput>
definitionHeaderInput(
    const QList<QLineEdit *>& lineEdits,
    const QList<QTextEdit *>& textEdits)
{
    QHash<QString, QString> fields;
    for (const QLineEdit *editor : lineEdits)
    {
        fields.insert(editor->objectName(), editor->text());
    }
    for (const QTextEdit *editor : textEdits)
    {
        fields.insert(editor->objectName(), editor->toPlainText());
    }

    std::optional<std::uint64_t> internalIdAddress;
    const QString addressText = fields.value("internalidaddress").trimmed();
    if (!addressText.isEmpty())
    {
        bool validAddress = false;
        const std::uint64_t parsedAddress = addressText.toULongLong(&validAddress, 16);
        if (!validAddress)
        {
            return fastecu::fail(
                fastecu::ErrorKind::InvalidConfig,
                "definition internal ID address is not a valid integer");
        }
        internalIdAddress = parsedAddress;
    }

    return fastecu::definition::DefinitionHeaderInput{
        .xml_id = fields.value("xmlid").trimmed().toStdString(),
        .internal_id = fields.value("internalidstring").toStdString(),
        .ecu_id = fields.value("ecuid").toStdString(),
        .internal_id_address = internalIdAddress,
        .metadata =
            fastecu::definition::RomMetadata{
                .make = fields.value("make").toStdString(),
                .market = fields.value("market").toStdString(),
                .model = fields.value("model").toStdString(),
                .submodel = fields.value("submodel").toStdString(),
                .transmission = fields.value("transmission").toStdString(),
                .year = fields.value("year").toStdString(),
                .flash_method = fields.value("flashmethod").toStdString(),
                .memory_model = fields.value("memmodel").toStdString(),
                .checksum_module = fields.value("checksummodule").toStdString(),
            },
        .include = fields.value("include").toStdString(),
        .notes = fields.value("notes").toStdString(),
    };
}

fastecu::Result<DefinitionCatalog> catalogFromLegacyLists(
    const FileActions::ConfigValuesStructure& config,
    DefinitionFormat format)
{
    const QStringList *ids =
        format == DefinitionFormat::RomRaider
            ? &config.romraider_def_cal_id
            : &config.ecuflash_def_cal_id;
    const QStringList *addresses =
        format == DefinitionFormat::RomRaider
            ? &config.romraider_def_cal_id_addr
            : &config.ecuflash_def_cal_id_addr;
    const QStringList *ecuIds =
        format == DefinitionFormat::RomRaider
            ? &config.romraider_def_ecu_id
            : &config.ecuflash_def_ecu_id;
    const QStringList *sources =
        format == DefinitionFormat::RomRaider
            ? &config.romraider_def_filename
            : &config.ecuflash_def_filename;

    const bool completeShape =
        sources->size() == ids->size() &&
        addresses->size() == ids->size() &&
        ecuIds->size() == ids->size();
    const bool readOnlyShape =
        sources->size() == ids->size() &&
        addresses->isEmpty() &&
        ecuIds->isEmpty();
    if (!completeShape && !readOnlyShape)
    {
        return fastecu::fail(
            fastecu::ErrorKind::InvalidConfig,
            "legacy definition catalog ID/source/address/ECU columns are not "
            "aligned; expected four complete columns or the documented "
            "ID/source-only read shape");
    }

    std::vector<DefinitionIndexEntry> entries;
    entries.reserve(static_cast<std::size_t>(ids->size()));
    for (qsizetype index = 0; index < ids->size(); ++index)
    {
        std::optional<std::uint64_t> address;
        const QString addressText =
            readOnlyShape ? QString{} : addresses->at(index);
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
            .ecu_id =
                readOnlyShape ? std::string{} : ecuIds->at(index).toStdString(),
            .source = sources->at(index).toStdString(),
        });
    }
    return DefinitionCatalog::create(std::move(entries));
}

void validateListLength(const QString& group,
                        const QString& name,
                        int actual,
                        int expected,
                        QStringList *errors)
{
    if (actual == expected)
    {
        return;
    }
    errors->append(QString("%1.%2 has %3 entries, expected %4")
                       .arg(group, name)
                       .arg(actual)
                       .arg(expected));
}

bool validateRequiredField(const QString& group,
                           const QString& name,
                           const QStringList& values,
                           QStringList *errors)
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
                         fastecu::IFileRepository& file_repository,
                         fastecu::IAtomicFileWriter& atomic_file_writer,
                         QWidget *parent)
    : QWidget(parent),
      configAdapter_(file_system, resource_bundle, file_repository),
      definitionFileSystem_(file_system),
      definitionFileRepository_(file_repository),
      definitionService_(file_system, file_repository, atomic_file_writer),
      definitionAdapter_(definitionService_),
      calibrationAdapter_(file_repository)
{
}

fastecu::Result<DefinitionCatalog> FileActions::build_definition_catalog(
    DefinitionFormat format)
{
    if (format == DefinitionFormat::RomRaider &&
        !ConfigValuesStruct.romraider_definition_files.isEmpty())
    {
        std::vector<std::string> handles;
        handles.reserve(static_cast<std::size_t>(
            ConfigValuesStruct.romraider_definition_files.size()));
        for (const QString& handle :
             ConfigValuesStruct.romraider_definition_files)
        {
            handles.push_back(handle.toStdString());
        }
        return definitionService_.build_romraider_catalog(handles);
    }
    if (format == DefinitionFormat::EcuFlash)
    {
        std::vector<std::string> explicitHandles;
        explicitHandles.reserve(static_cast<std::size_t>(
            ConfigValuesStruct.ecuflash_def_filename.size()));
        for (const QString& handle :
             ConfigValuesStruct.ecuflash_def_filename)
        {
            explicitHandles.push_back(handle.toStdString());
        }
        if (ConfigValuesStruct.ecuflash_definition_files_directory.isEmpty() &&
            explicitHandles.empty())
        {
            return catalogFromLegacyLists(ConfigValuesStruct, format);
        }
        return definitionService_.build_ecuflash_catalog(
            ConfigValuesStruct.ecuflash_definition_files_directory.toStdString(),
            explicitHandles);
    }
    return catalogFromLegacyLists(ConfigValuesStruct, format);
}

QString FileActions::definition_source(
    DefinitionFormat format,
    const QString& id) const
{
    const QStringList *ids =
        format == DefinitionFormat::RomRaider
            ? &ConfigValuesStruct.romraider_def_cal_id
            : &ConfigValuesStruct.ecuflash_def_cal_id;
    const QStringList *sources =
        format == DefinitionFormat::RomRaider
            ? &ConfigValuesStruct.romraider_def_filename
            : &ConfigValuesStruct.ecuflash_def_filename;
    const qsizetype index = ids->indexOf(id);
    return index >= 0 && index < sources->size()
               ? sources->at(index)
               : QString{};
}

void FileActions::log_definition_error(
    const QString& operation,
    const fastecu::Error& error)
{
    emit LOG_E(
        operation + " [" +
            QString::fromUtf8(fastecu::to_string(error.kind)) + "]: " +
            QString::fromStdString(error.detail),
        true,
        true);
}

fastecu::Status FileActions::load_configured_definition(
    EcuCalDefStructure& ecu_cal_def,
    DefinitionFormat format,
    const QString& definition_id)
{
    auto catalog = build_definition_catalog(format);
    if (!catalog.has_value())
    {
        resolvedDefinition_.reset();
        return std::unexpected(catalog.error());
    }
    fastecu::definition::RomDefinition resolved;
    fastecu::Status replaced = definitionAdapter_.replace_definition(
        ecu_cal_def, *catalog, format, definition_id.toStdString(), &resolved);
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

const fastecu::definition::RomDefinition *FileActions::resolved_definition(
    DefinitionFormat format,
    const QString& definition_id) const
{
    if (!resolvedDefinition_.has_value() ||
        resolvedDefinition_->format != format ||
        resolvedDefinition_->id != definition_id)
    {
        return nullptr;
    }
    return &resolvedDefinition_->definition;
}

bool FileActions::log_definition_load_failure(
    const QString& operation,
    const fastecu::Error& error,
    const QString& source,
    const QString& warning_title,
    const QString& warning_text)
{
    log_definition_error(operation, error);
    if (!source.isEmpty() &&
        !definitionFileSystem_.exists(source.toStdString()))
    {
        QMessageBox::warning(this, warning_title, warning_text + source + " for reading");
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

fastecu::Status FileActions::submit_new_definition(
    std::string_view destination,
    const fastecu::definition::DefinitionHeaderInput& input)
{
    // The caller reaches this only after the native "Save As" dialog already asked to
    // overwrite an existing file, so that confirmation is passed through here rather than
    // rejected again by the service's own new-file-only guard.
    fastecu::Status status =
        definitionAdapter_.create_definition(destination, input, /*allow_overwrite=*/true);
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

fastecu::Status FileActions::submit_imported_definition(
    std::string_view source,
    std::string_view destination,
    const fastecu::definition::DefinitionHeaderInput& input)
{
    fastecu::Status status =
        definitionAdapter_.import_definition(source, destination, input);
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

void FileActions::remember_submitted_ecuflash_handle(
    std::string_view destination)
{
    const auto position = std::ranges::lower_bound(
        submittedEcuflashHandles_,
        destination,
        {},
        [](const auto& item)
        { return std::string_view{item}; });
    if (position == std::ranges::end(submittedEcuflashHandles_) ||
        std::string_view(*position) != destination)
    {
        submittedEcuflashHandles_.emplace(
            position,
            destination);
    }
}

void FileActions::apply_flash_method_alias(EcuCalDefStructure& ecuCalDef)
{
    if (ecuCalDef.RomInfo.size() <= FlashMethod)
    {
        return;
    }
    const QString flashMethod = ecuCalDef.RomInfo.at(FlashMethod);
    for (qsizetype index = 0;
         index < ConfigValuesStruct.flash_protocol_id.size() &&
         index < ConfigValuesStruct.flash_protocol_alias.size() &&
         index < ConfigValuesStruct.flash_protocol_protocol_name.size();
         ++index)
    {
        const QStringList aliases =
            ConfigValuesStruct.flash_protocol_alias.at(index).split(",");
        if (aliases.contains(flashMethod))
        {
            emit LOG_D("Alias: " + flashMethod, true, true);
            emit LOG_D(
                "Protocol: " +
                    ConfigValuesStruct.flash_protocol_protocol_name.at(index),
                true,
                true);
            ecuCalDef.RomInfo.replace(
                FlashMethod,
                ConfigValuesStruct.flash_protocol_protocol_name.at(index));
            return;
        }
    }
}

void FileActions::normalize_definition_addresses(
    EcuCalDefStructure& ecuCalDef)
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
    for (QStringList *addresses :
         {&ecuCalDef.AddressList,
          &ecuCalDef.XScaleAddressList,
          &ecuCalDef.YScaleAddressList})
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
    validateListLength("flash_protocol", "flash_transport", configValues.flash_protocol_flash_transport.size(), rows, out);
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
    validateListLength("log_value", "units", logValues.log_value_units.size(), rows, out);
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
                                                             const QStringList& defData,
                                                             int *endIndex)
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
            const QDomElement element = (name == "include" || name == "notes")
                                            ? root.firstChildElement(name)
                                            : romid.firstChildElement(name);
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

FileActions::ConfigValuesStructure *FileActions::set_base_dirs(
    ConfigValuesStructure *configValues, const fastecu::config::AppRootInfo& root_info)
{
    return configAdapter_.set_base_dirs(configValues, root_info);
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

FileActions::LogValuesStructure *FileActions::read_logger_conf(FileActions::LogValuesStructure *logValues, const QString& ecu_id, bool modify)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    QDomDocument xmlBOM;

    QString filename = configValues->logger_file;

    emit LOG_D("Looking for ECU ID: " + ecu_id + " in logger def file: " + configValues->logger_file, true, true);

    QFile file(filename);
    if (!file.open(QFile::ReadWrite | QFile::Text))
    {
        QMessageBox::warning(this, tr("Logger file"), "Unable to open logger config file '" + file.fileName() + "' for reading");
        return nullptr;
    }
    xmlBOM.setContent(&file);

    if (!modify)
    {
        logValues->dashboard_log_value_id.clear();
        logValues->lower_panel_log_value_id.clear();
        logValues->lower_panel_switch_id.clear();
    }

    bool ecu_id_found = false;
    int index = 0;

    QDomElement root = xmlBOM.documentElement();

    if (root.tagName() == "config")
    {
        QDomElement logger = root.firstChild().toElement();
        if (logger.tagName() == "logger")
        {
            QDomElement ecu = logger.firstChild().toElement();
            while (!ecu.isNull())
            {
                if (ecu.tagName() == "ecu")
                {
                    QString file_ecu_id = ecu.attribute("id", "No id");
                    QString ecu_id_active = ecu.attribute("active", "false");
                    if (ecu_id == file_ecu_id)
                    {
                        ecu_id_found = true;
                        emit LOG_D("Found ECU ID " + file_ecu_id, true, true);
                        QDomElement protocol = ecu.firstChild().toElement();
                        while (!protocol.isNull())
                        {
                            if (protocol.tagName() == "protocol")
                            {
                                emit LOG_D("Found protocol " + protocol.attribute("id", "No id"), true, true);
                                logValues->logging_values_protocol = protocol.attribute("id", "No id");
                                QDomElement parameters = protocol.firstChild().toElement();
                                while (!parameters.isNull())
                                {
                                    if (parameters.tagName() == "parameters")
                                    {
                                        QDomElement parameter_type = parameters.firstChild().toElement();
                                        while (!parameter_type.isNull())
                                        {
                                            if (parameter_type.tagName() == "gauges")
                                            {
                                                index = 0;
                                                QDomElement gauges = parameter_type.firstChild().toElement();
                                                while (!gauges.isNull())
                                                {
                                                    if (gauges.tagName() == "parameter")
                                                    {
                                                        if (!modify)
                                                        {
                                                            logValues->dashboard_log_value_id.append(gauges.attribute("id", "No id"));
                                                        }
                                                        else
                                                        {
                                                            gauges.setAttribute("id", logValues->dashboard_log_value_id.at(index));
                                                        }
                                                    }
                                                    gauges = gauges.nextSibling().toElement();
                                                    index++;
                                                }
                                            }
                                            if (parameter_type.tagName() == "lower_panel")
                                            {
                                                index = 0;
                                                QDomElement lower_panel = parameter_type.firstChild().toElement();
                                                while (!lower_panel.isNull())
                                                {
                                                    if (lower_panel.tagName() == "parameter")
                                                    {
                                                        if (!modify)
                                                        {
                                                            logValues->lower_panel_log_value_id.append(lower_panel.attribute("id", "No id"));
                                                        }
                                                        else
                                                        {
                                                            QDomElement parameter = xmlBOM.createElement("parameter");
                                                            parameter.setAttribute("id", logValues->lower_panel_log_value_id.at(index));
                                                            parameter.setAttribute("name", "");

                                                            lower_panel.setAttribute("id", logValues->lower_panel_log_value_id.at(index));
                                                        }
                                                    }
                                                    lower_panel = lower_panel.nextSibling().toElement();
                                                    index++;
                                                }
                                            }
                                            parameter_type = parameter_type.nextSibling().toElement();
                                        }
                                    }
                                    if (parameters.tagName() == "switches")
                                    {
                                        index = 0;
                                        QDomElement switches = parameters.firstChild().toElement();
                                        while (!switches.isNull())
                                        {
                                            if (switches.tagName() == "switch")
                                            {
                                                if (!modify)
                                                {
                                                    logValues->lower_panel_switch_id.append(switches.attribute("id", "No id"));
                                                }
                                                else
                                                {
                                                    switches.setAttribute("id", logValues->lower_panel_switch_id.at(index));
                                                }
                                            }
                                            switches = switches.nextSibling().toElement();
                                            index++;
                                        }
                                    }
                                    parameters = parameters.nextSibling().toElement();
                                }
                            }
                            protocol = protocol.nextSibling().toElement();
                        }
                    }
                }
                ecu = ecu.nextSibling().toElement();
            }
        }
        if (!ecu_id_found)
        {
            if (logValues->log_value_protocol.empty())
            {
                QMessageBox::warning(this, tr("Logger definition file"), "No logger definition file selected, returning without initializing log parameters!");
                emit LOG_D("No logger definition file selected, returning without initializing log parameters!", true, true);
                return 0;
            }
            emit LOG_D("ECU ID not found, initializing log parameters", true, true);
            logValues->logging_values_protocol = logValues->log_value_protocol.at(0);
            emit LOG_D("Initializing gauge parameters", true, true);
            for (int i = 0; i < logValues->log_value_id.length(); i++)
            {
                if (logValues->log_value_enabled.at(i) == "1" && logValues->dashboard_log_value_id.length() < 15)
                {
                    logValues->dashboard_log_value_id.append(logValues->log_value_id.at(i));
                }
            }
            emit LOG_D("Initializing lower panel parameters", true, true);
            for (int i = 0; i < logValues->log_value_id.length(); i++)
            {
                if (logValues->log_value_enabled.at(i) == "1" && logValues->lower_panel_log_value_id.length() < 12)
                {
                    logValues->lower_panel_log_value_id.append(logValues->log_value_id.at(i));
                }
            }
            emit LOG_D("Initializing switch parameters", true, true);
            for (int i = 0; i < logValues->log_switch_id.length(); i++)
            {
                if (logValues->log_switch_enabled.at(i) == "1" && logValues->lower_panel_switch_id.length() < 20)
                {
                    logValues->lower_panel_switch_id.append(logValues->log_switch_id.at(i));
                }
            }
            emit LOG_D("Values initialized, creating xml data", true, true);
            // save_logger_conf(logValues, ecu_id);
            QDomElement ecu = xmlBOM.createElement("ecu");
            ecu.setAttribute("id", ecu_id);
            logger.appendChild(ecu);
            QDomElement protocol = xmlBOM.createElement("protocol");
            protocol.setAttribute("id", logValues->logging_values_protocol);
            ecu.appendChild(protocol);
            QDomElement parameters = xmlBOM.createElement("parameters");
            protocol.appendChild(parameters);
            QDomElement gauges = xmlBOM.createElement("gauges");
            parameters.appendChild(gauges);
            for (int i = 0; i < logValues->dashboard_log_value_id.length(); i++)
            {
                QDomElement parameter = xmlBOM.createElement("parameter");
                gauges.appendChild(parameter);
                parameter.setAttribute("id", logValues->dashboard_log_value_id.at(i));
                parameter.setAttribute("name", "");
            }
            QDomElement lower_panel = xmlBOM.createElement("lower_panel");
            parameters.appendChild(lower_panel);
            for (int i = 0; i < logValues->lower_panel_log_value_id.length(); i++)
            {
                QDomElement parameter = xmlBOM.createElement("parameter");
                lower_panel.appendChild(parameter);
                parameter.setAttribute("id", logValues->lower_panel_log_value_id.at(i));
                parameter.setAttribute("name", "");
            }
            QDomElement switches = xmlBOM.createElement("switches");
            protocol.appendChild(switches);
            for (int i = 0; i < logValues->lower_panel_switch_id.length(); i++)
            {
                QDomElement parameter = xmlBOM.createElement("switch");
                switches.appendChild(parameter);
                parameter.setAttribute("id", logValues->lower_panel_switch_id.at(i));
                parameter.setAttribute("name", "");
            }
            // emit LOG_D("Saving log parameters", true, true);
            file.resize(0);
            QTextStream output(&file);
            xmlBOM.save(output, 4);
            file.close();
        }
    }
    if (modify)
    {
        file.resize(0);
        QTextStream output(&file);
        xmlBOM.save(output, 4);
        // output << xmlBOM.toString();
    }
    file.close();

    return logValues;
}
/*
void *FileActions::save_logger_conf(FileActions::LogValuesStructure *logValues, QString ecu_id)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    QFile file(configValues->logger_file);
    if (!file.open(QIODevice::ReadWrite)) {
        QMessageBox::warning(this, tr("Config file"), "Unable to open logger config file '" + file.fileName() + "' for writing");
        return 0;
    }
    QXmlStreamReader reader;
    reader.setDevice(&file);

    QXmlStreamWriter stream(&file);
    //file.resize(0);
    stream.setAutoFormatting(true);
    stream.writeStartDocument();
    stream.writeStartElement("config");
    stream.writeAttribute("name", configValues->software_title);
    stream.writeAttribute("version", configValues->software_version);
    emit LOG_D("Software version: " + configValues->software_version;
    stream.writeStartElement("logger");
    stream.writeStartElement("ecu");
    stream.writeAttribute("id", ecu_id);
    stream.writeStartElement("protocol");
    stream.writeAttribute("id", logValues->logging_values_protocol);
    stream.writeStartElement("parameters");
    stream.writeStartElement("gauges");
    for (int i = 0; i < 15; i++)
    {
        stream.writeStartElement("parameter");
        stream.writeAttribute("id", logValues->dashboard_log_value_id.at(i));
        stream.writeAttribute("name", "");
        stream.writeEndElement();
    }
    stream.writeEndElement();
    stream.writeStartElement("lower_panel");
    for (int i = 0; i < 15; i++)
    {
        stream.writeStartElement("parameter");
        stream.writeAttribute("id", logValues->dashboard_log_value_id.at(i));
        stream.writeAttribute("name", "");
        stream.writeEndElement();
    }
    stream.writeEndElement();
    stream.writeEndElement();
    stream.writeStartElement("switches");
    for (int i = 0; i < 15; i++)
    {
        stream.writeStartElement("switch");
        stream.writeAttribute("id", logValues->dashboard_log_value_id.at(i));
        stream.writeAttribute("name", "");
        stream.writeEndElement();
    }
    stream.writeEndElement();
    stream.writeEndElement();
    stream.writeEndElement();
    stream.writeEndElement();
    stream.writeEndElement();

    return 0;
}
*/
FileActions::LogValuesStructure *FileActions::read_logger_definition_file()
{
    LogValuesStructure *logValues = &LogValuesStruct;
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    // The QDomDocument class represents an XML document.
    QDomDocument xmlBOM;

    QString filename = configValues->romraider_logger_definition_file;
    if (filename.isEmpty() && configValues->flash_protocol_selected_log_protocol == "CDBG")
    {
        const QString userCdbgLogger = configValues->config_files_directory + "logger_cdbg_example.xml";
        filename = QFileInfo::exists(userCdbgLogger)
                       ? userCdbgLogger
                       : QStringLiteral(":/config/logger_cdbg_example.xml");
        configValues->romraider_logger_definition_file = filename;
        emit LOG_D("Using bundled CDBG logger definition: " + filename, true, true);
    }
    // emit LOG_D("Logger filename = " + filename;
    QFile file(filename);
    if (!file.open(QFile::ReadOnly | QFile::Text))
    {
        QMessageBox::warning(this, tr("Logger file"), "Unable to open logger definition file '" + file.fileName() + "' for reading");
        return logValues;
    }
    xmlBOM.setContent(&file);
    file.close();

    // Extract the root markup
    QDomElement root = xmlBOM.documentElement();

    // Get root names and attributes
    // QString Type = root.tagName();
    // QString Name = root.attribute("name","No name");

    int log_value_index = 0;
    int log_switch_index = 0;

    if (root.tagName() == "logger")
    {
        // emit LOG_D("Logger start element";
        QDomElement protocols = root.firstChild().toElement();

        while (!protocols.isNull())
        {
            // emit LOG_D("Protocols start element";
            if (protocols.tagName() == "protocols")
            {
                QDomElement protocol = protocols.firstChild().toElement();
                while (!protocol.isNull())
                {
                    if (protocol.tagName() == "protocol")
                    {
                        QString log_value_protocol = protocol.attribute("id", "No protocol id");
                        // emit LOG_D("Protocol = " + protocol.attribute("id","No id");
                        QDomElement transports = protocol.firstChild().toElement();
                        while (!transports.isNull())
                        {
                            if (transports.tagName() == "transports")
                            {
                                // emit LOG_D("Transports for protocol " + protocol.attribute("id","No id");
                                QDomElement transport = transports.firstChild().toElement();
                                while (!transport.isNull())
                                {
                                    if (transport.tagName() == "transport")
                                    {
                                        // emit LOG_D("Transport = " + transport.attribute("id","No id") + " " + transport.attribute("name","No name") + " " + transport.attribute("desc","No description");
                                    }
                                    QDomElement module = transport.firstChild().toElement();
                                    while (!module.isNull())
                                    {
                                        if (module.tagName() == "module")
                                        {
                                            // emit LOG_D("Module = " + module.attribute("id","No id") + " " + module.attribute("address","No address") + " " + module.attribute("desc","No description") + " " + module.attribute("tester","No tester");
                                        }
                                        module = module.nextSibling().toElement();
                                    }
                                    transport = transport.nextSibling().toElement();
                                }
                            }
                            if (transports.tagName() == "parameters")
                            {
                                QDomElement parameter = transports.firstChild().toElement();
                                while (!parameter.isNull())
                                {
                                    if (parameter.tagName() == "parameter")
                                    {
                                        QString log_value_id = parameter.attribute("id", "No id");
                                        // emit LOG_D("Parameter = " + parameter.attribute("id","No id") + " " + parameter.attribute("name","No name") + " " + parameter.attribute("desc","No description");
                                        logValues->log_value_protocol.append(log_value_protocol);
                                        logValues->log_value_id.append(parameter.attribute("id", "No id"));
                                        logValues->log_value_name.append(parameter.attribute("name", "No name"));
                                        logValues->log_value_description.append(parameter.attribute("desc", "No desc"));
                                        logValues->log_value_ecu_byte_index.append(parameter.attribute("ecubyteindex", "No byte index"));
                                        logValues->log_value_ecu_bit.append(parameter.attribute("ecubit", "No ecu bit"));
                                        logValues->log_value_target.append(parameter.attribute("target", "No target"));
                                        logValues->log_value_enabled.append(parameter.attribute("enabled", "0"));
                                        logValues->log_value.append("0.00");
                                        if (log_value_index < 12)
                                        {
                                            logValues->lower_panel_log_value_id.append(log_value_id);
                                        }
                                        if (log_value_index < 15)
                                        {
                                            logValues->dashboard_log_value_id.append(log_value_id);
                                        }

                                        QDomElement param_options = parameter.firstChild().toElement();
                                        while (!param_options.isNull())
                                        {
                                            if (param_options.tagName() == "address")
                                            {
                                                // emit LOG_D("Address = " + param_options.text();
                                                logValues->log_value_length.append(parameter.attribute("length", "1"));
                                                logValues->log_value_address.append(param_options.text());
                                            }
                                            if (param_options.tagName() == "conversions")
                                            {
                                                QDomElement conversion = param_options.firstChild().toElement();
                                                QString param_conversion;
                                                int conversion_count = 0;
                                                while (!conversion.isNull())
                                                {
                                                    if (conversion.tagName() == "conversion")
                                                    {
                                                        // emit LOG_D("Conversion = " + conversion.attribute("units","No units") + " " + conversion.attribute("expr","No expr") + " " + conversion.attribute("format","No format") + " " + conversion.attribute("gauge_min","0") + " " + conversion.attribute("gauge_max","0") + " " + conversion.attribute("gauge_step","0");
                                                        param_conversion.append("conversion " + QString::number(conversion_count) + ",");
                                                        param_conversion.append(conversion.attribute("units", "#") + ",");
                                                        param_conversion.append(conversion.attribute("expr", "x") + ",");
                                                        param_conversion.append(conversion.attribute("format", "0.00") + ",");
                                                        param_conversion.append(conversion.attribute("gauge_min", "No gauge_min") + ",");
                                                        param_conversion.append(conversion.attribute("gauge_max", "No gauge_max") + ",");
                                                        param_conversion.append(conversion.attribute("gauge_step", "No gauge_step"));
                                                    }
                                                    conversion_count++;
                                                    conversion = conversion.nextSibling().toElement();
                                                    if (!conversion.isNull())
                                                    {
                                                        param_conversion.append(",");
                                                    }
                                                }
                                                logValues->log_value_units.append(param_conversion);
                                            }
                                            param_options = param_options.nextSibling().toElement();
                                        }
                                        log_value_index++;
                                    }
                                    parameter = parameter.nextSibling().toElement();
                                }
                            }
                            if (transports.tagName() == "switches")
                            {
                                QDomElement paramswitch = transports.firstChild().toElement();
                                while (!paramswitch.isNull())
                                {
                                    if (paramswitch.tagName() == "switch")
                                    {
                                        QString log_switch_id = paramswitch.attribute("id", "No id");
                                        logValues->log_switch_protocol.append(log_value_protocol);
                                        logValues->log_switch_id.append(paramswitch.attribute("id", "No id"));
                                        logValues->log_switch_name.append(paramswitch.attribute("name", "No name"));
                                        logValues->log_switch_description.append(paramswitch.attribute("desc", "No desc"));
                                        logValues->log_switch_address.append(paramswitch.attribute("byte", "No address"));
                                        logValues->log_switch_ecu_byte_index.append(paramswitch.attribute("ecubyteindex", "No ecu byte index"));
                                        logValues->log_switch_ecu_bit.append(paramswitch.attribute("bit", "No ecu bit"));
                                        logValues->log_switch_target.append(paramswitch.attribute("target", "No target"));
                                        logValues->log_switch_enabled.append("0");
                                        logValues->log_switch_state.append("0");
                                        if (log_switch_index < 20)
                                        {
                                            logValues->lower_panel_switch_id.append(log_switch_id);
                                        }
                                        log_switch_index++;
                                        // emit LOG_D("Switch = " + paramswitch.attribute("id","No id") + " " + paramswitch.attribute("name","No name") + " " + paramswitch.attribute("desc","No description");
                                    }
                                    paramswitch = paramswitch.nextSibling().toElement();
                                }
                            }
                            if (transports.tagName() == "dtcodes")
                            {
                                QDomElement dtcode = transports.firstChild().toElement();
                                while (!dtcode.isNull())
                                {
                                    if (dtcode.tagName() == "dtcode")
                                    {
                                        // emit LOG_D("DT code = " + dtcode.attribute("id","No id") + " " + dtcode.attribute("name","No name") + " " + dtcode.attribute("desc","No description");
                                    }
                                    dtcode = dtcode.nextSibling().toElement();
                                }
                            }
                            if (transports.tagName() == "ecuparams")
                            {
                                QDomElement ecuparam = transports.firstChild().toElement();
                                while (!ecuparam.isNull())
                                {
                                    if (ecuparam.tagName() == "ecuparam")
                                    {
                                        // emit LOG_D("ECU param = " + ecuparam.attribute("id","No id") + " " + ecuparam.attribute("name","No name") + " " + ecuparam.attribute("desc","No description");
                                    }
                                    ecuparam = ecuparam.nextSibling().toElement();
                                }
                            }
                            transports = transports.nextSibling().toElement();
                        }
                    }
                    protocol = protocol.nextSibling().toElement();
                }
            }
            protocols = protocols.nextSibling().toElement();
        }
    }

    // log_values_names_sorted
    // log_switch_names_sorted
    QStringList validationErrors;
    validate_logger_values(*logValues, &validationErrors);
    validate_logger_switches(*logValues, &validationErrors);
    if (!validationErrors.isEmpty())
    {
        logValidationErrors("Invalid logger definition:", validationErrors);
    }

    return logValues;
}

QSignalMapper *FileActions::read_menu_file(QMenuBar *menubar, QToolBar *toolBar)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    QMenu *mainWindowMenu;
    QMenu *subMenu;

    QSignalMapper *mapper = new QSignalMapper(this);
    bool toolBarIconSet = false;

    // The QDomDocument class represents an XML document.
    QDomDocument xmlBOM;
    // Load xml file as raw data
    QFile file(configValues->menu_file);
    if (!file.open(QIODevice::ReadOnly))
    {
        // Error while loading file
        QMessageBox::warning(this, tr("Ecu menu file"), "Unable to open menu config file '" + file.fileName() + "' for reading");
        return mapper;
    }
    // Set data into the QDomDocument before processing
    xmlBOM.setContent(&file);
    file.close();

    // Extract the root markup
    QDomElement root = xmlBOM.documentElement();

    // Get root names and attributes
    QString Type = root.tagName();
    QString Name = root.attribute("name", "No name");

    QDomElement TagType = root.firstChild().toElement();

    while (!TagType.isNull())
    {
        if (TagType.tagName() == "ecu_menu_definitions")
        {
            QDomElement Component = TagType.firstChild().toElement();
            while (!Component.isNull())
            {
                // Check if the child tag name is categories
                if (Component.tagName() == "menu")
                {
                    QString menuName = Component.attribute("name", "No name");
                    mainWindowMenu = menubar->addMenu(menuName);
                    // emit LOG_D("Menu:  " + menuName;

                    QDomElement menu_item = Component.firstChild().toElement();
                    while (!menu_item.isNull())
                    {
                        // Check if the child tag name is table
                        if (menu_item.tagName() == "menu")
                        {
                            QString subMenuName = menu_item.attribute("name", "No name");
                            subMenu = mainWindowMenu->addMenu(subMenuName);
                            QDomElement sub_menu_item = menu_item.firstChild().toElement();
                            while (!sub_menu_item.isNull())
                            {
                                if (sub_menu_item.tagName() == "menuitem")
                                {
                                    QString menuItemName = sub_menu_item.attribute("name", "No name");
                                    if (menuItemName == "Separator")
                                    {
                                        subMenu->addSeparator();
                                    }
                                    else
                                    {
                                        QAction *action = new QAction(menuItemName, this);
                                        QString menuItemActionName = sub_menu_item.attribute("id", "No id");
                                        ;
                                        QString menuItemCheckable = sub_menu_item.attribute("checkable", "No checkable");
                                        ;
                                        QString menuItemShortcut = sub_menu_item.attribute("shortcut", "No shortcut");
                                        ;
                                        QString menuItemToolbar = sub_menu_item.attribute("toolbar", "No toolbar");
                                        ;
                                        QString menuItemIcon = sub_menu_item.attribute("icon", "No icon");
                                        ;
                                        QString menuItemTooltip = sub_menu_item.attribute("tooltip", "No tooltip");
                                        ;

                                        action->setObjectName(menuItemActionName);
                                        // emit LOG_D(menuItemShortcut;
                                        action->setShortcut(menuItemShortcut);
                                        action->setIcon(QIcon(menuItemIcon));
                                        action->setIconVisibleInMenu(true);
                                        action->setToolTip(subMenuName + "\n\n" + menuItemTooltip);
                                        if (menuItemCheckable == "true")
                                        {
                                            action->setCheckable(true);
                                        }
                                        else
                                        {
                                            action->setCheckable(false);
                                        }
                                        if (menuItemToolbar == "true")
                                        {
                                            toolBar->addAction(action);
                                            toolBarIconSet = true;
                                        }

                                        subMenu->addAction(action);
                                        mapper->setMapping(action, action->objectName());
                                        connect(action, SIGNAL(triggered(bool)), mapper, SLOT(map()));
                                    }
                                }

                                sub_menu_item = sub_menu_item.nextSibling().toElement();
                            }
                        }
                        if (menu_item.tagName() == "menuitem")
                        {
                            QString menuItemName = menu_item.attribute("name", "No name");
                            if (menuItemName == "Separator")
                            {
                                mainWindowMenu->addSeparator();
                            }
                            else
                            {
                                QAction *action = new QAction(menuItemName, this);
                                QString menuItemActionName = menu_item.attribute("id", "No id");
                                ;
                                QString menuItemCheckable = menu_item.attribute("checkable", "No checkable");
                                ;
                                QString menuItemShortcut = menu_item.attribute("shortcut", "No shortcut");
                                ;
                                QString menuItemToolbar = menu_item.attribute("toolbar", "No toolbar");
                                ;
                                QString menuItemIcon = menu_item.attribute("icon", "No icon");
                                ;
                                QString menuItemTooltip = menu_item.attribute("tooltip", "No tooltip");
                                ;

                                action->setObjectName(menuItemActionName);
                                // emit LOG_D(menuItemShortcut;
                                action->setShortcut(menuItemShortcut);
                                action->setIcon(QIcon(menuItemIcon));
                                action->setIconVisibleInMenu(true);
                                action->setToolTip(menuItemName + "\n\n" + menuItemTooltip);
                                if (menuItemCheckable == "true")
                                {
                                    action->setCheckable(true);
                                }
                                else
                                {
                                    action->setCheckable(false);
                                }
                                if (menuItemToolbar == "true")
                                {
                                    toolBar->addAction(action);
                                    toolBarIconSet = true;
                                }

                                mainWindowMenu->addAction(action);
                                mapper->setMapping(action, action->objectName());
                                connect(action, SIGNAL(triggered(bool)), mapper, SLOT(map()));
                            }
                        }
                        menu_item = menu_item.nextSibling().toElement();
                    }
                    if (toolBarIconSet)
                    {
                        toolBar->addSeparator();
                    }
                    toolBarIconSet = false;
                }
                Component = Component.nextSibling().toElement();
            }
        }
        /*
                if (TagType.tagName() == "popup_menu_definitions")
                {
                    QDomElement Component = TagType.firstChild().toElement();
                    while(!Component.isNull())
                    {
                        if (Component.tagName() == "menu")
                        {
                            QDomElement Child = Component.firstChild().toElement();
                            while(!Child.isNull())
                            {
                                if (Child.tagName() == "menuitem")
                                {
                                    popupMenuItemName.append(Child.attribute("name","No name"));
                                    popupMenuItemActionName.append(Child.attribute("id","No id"));
                                    popupMenuItemCheckable.append(Child.attribute("checkable","No checkable"));
                                    popupMenuItemShortcut.append(Child.attribute("shortcut","No shortcut"));
                                    popupMenuItemToolbar.append(Child.attribute("toolbar","No toolbar"));
                                    popupMenuItemIcon.append(Child.attribute("icon","No icon"));
                                    popupMenuItemTooltip.append(Child.attribute("tooltip","No tooltip"));
                                }
                                Child = Child.nextSibling().toElement();
                            }
                            if (toolBarIconSet)
                                ui->mainToolBar->addSeparator();
                            toolBarIconSet = false;
                        }
                        Component = Component.nextSibling().toElement();
                    }
                }
        */
        TagType = TagType.nextSibling().toElement();
    }
    // connect(mapper, SIGNAL(mapped(QString)), this, SLOT(menuActionTriggered(QString)));

    return mapper;
}

QString FileActions::parse_hex_ecuid(uint8_t byte)
{
    QString ecuid_byte;
    unsigned char chars[] = "0123456789ABCDEF";

    ecuid_byte = (QChar)chars[(byte >> 4) & 0xF];
    ecuid_byte.append((QChar)chars[(byte >> 0) & 0xF]);
    // emit LOG_D("Constructed byte: " + ecuid_byte;

    return ecuid_byte;
}

FileActions::EcuCalDefStructure *FileActions::parse_ecuid_ecuflash_def_files(FileActions::EcuCalDefStructure *ecuCalDef, bool is_ascii)
{
    (void)is_ascii;
    auto catalog =
        build_definition_catalog(DefinitionFormat::EcuFlash);
    if (!catalog.has_value())
    {
        log_definition_error("Unable to match EcuFlash definition", catalog.error());
        return ecuCalDef;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(
        ecuCalDef->FullRomData.constData());
    auto match = definitionService_.match_rom(
        *catalog,
        std::span<const std::uint8_t>(
            bytes,
            static_cast<std::size_t>(ecuCalDef->FullRomData.size())));
    if (!match.has_value())
    {
        log_definition_error("Unable to match EcuFlash definition", match.error());
        return ecuCalDef;
    }
    ecuCalDef->RomId = QString::fromStdString(match->definition_id);
    emit LOG_D(
        "EcuFlash cal id " + ecuCalDef->RomId + " found",
        true,
        true);
    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::parse_ecuid_romraider_def_files(FileActions::EcuCalDefStructure *ecuCalDef, bool is_ascii)
{
    (void)is_ascii;
    auto catalog =
        build_definition_catalog(DefinitionFormat::RomRaider);
    if (!catalog.has_value())
    {
        log_definition_error("Unable to match RomRaider definition", catalog.error());
        return ecuCalDef;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(
        ecuCalDef->FullRomData.constData());
    auto match = definitionService_.match_rom(
        *catalog,
        std::span<const std::uint8_t>(
            bytes,
            static_cast<std::size_t>(ecuCalDef->FullRomData.size())));
    if (!match.has_value())
    {
        log_definition_error("Unable to match RomRaider definition", match.error());
        return ecuCalDef;
    }
    ecuCalDef->RomId = QString::fromStdString(match->definition_id);
    emit LOG_D(
        "RomRaider cal id " + ecuCalDef->RomId + " found",
        true,
        true);
    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::create_new_definition_for_rom(FileActions::EcuCalDefStructure *ecuCalDef)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    QString filename;
    QFileDialog saveDialog;
    bool isFileSelected = false;

    QDialog *definitionDialog = new QDialog(this);
    QVBoxLayout *vBoxLayout = new QVBoxLayout(definitionDialog);
    QLabel *label = new QLabel("Please provide ROM Information:");
    vBoxLayout->addWidget(label);

    QGridLayout *defHeaderGridLayout = new QGridLayout();
    QList<QLineEdit *> lineEditList;
    QList<QTextEdit *> textEditList;
    int index = 0;
    emit LOG_D("Create header", true, true);
    for (int i = 0; i < ecuCalDef->DefHeaderNames.length(); i++)
    {
        QLabel *label = new QLabel(ecuCalDef->DefHeaderStrings.at(index));
        defHeaderGridLayout->addWidget(label, index, 0);

        if (ecuCalDef->DefHeaderNames.at(i) == "notes")
        {
            textEditList.append(new QTextEdit());
            textEditList.at(textEditList.length() - 1)->setObjectName(ecuCalDef->DefHeaderNames.at(i));
            // textEditList.at(textEditList.length()-1)->setText(headerData.at(i+1));
            defHeaderGridLayout->addWidget(textEditList.at(textEditList.length() - 1), index + 1, 0, 1, 2);
        }
        else
        {
            lineEditList.append(new QLineEdit());
            lineEditList.at(lineEditList.length() - 1)->setObjectName(ecuCalDef->DefHeaderNames.at(i));
            // lineEditList.at(lineEditList.length()-1)->setText(headerData.at(i+1));
            defHeaderGridLayout->addWidget(lineEditList.at(lineEditList.length() - 1), index, 1);
        }
        index++;
    }
    vBoxLayout->addLayout(defHeaderGridLayout);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    vBoxLayout->addWidget(buttonBox);
    connect(buttonBox, &QDialogButtonBox::accepted, definitionDialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, definitionDialog, &QDialog::reject);

    definitionDialog->setMinimumWidth(500);
    int result = definitionDialog->exec();
    if (result == QDialog::Accepted)
    {
        // 0x0C6D57
        while (filename.isEmpty() && !isFileSelected)
        {
            saveDialog.setDefaultSuffix("xml");
            filename = QFileDialog::getSaveFileName(this, tr("Select definition file"), configValues->ecuflash_definition_files_directory, tr("Definition file (*.xml)"));
            if (filename.isEmpty())
            {
                QDialog *definitionDialog = new QDialog(this);
                QVBoxLayout *vBoxLayout = new QVBoxLayout(definitionDialog);
                QLabel *label = new QLabel("No file selected!\n\nIf you still want to create file click 'Ok'\nIf you want to continue to use ROM without definition, click 'Cancel'");
                QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
                connect(buttonBox, &QDialogButtonBox::accepted, definitionDialog, &QDialog::accept);
                connect(buttonBox, &QDialogButtonBox::rejected, definitionDialog, &QDialog::reject);

                vBoxLayout->addWidget(label);
                vBoxLayout->addWidget(buttonBox);

                int result = definitionDialog->exec();
                if (result == QDialog::Rejected)
                {
                    isFileSelected = true;
                }
            }
        }
        if (filename.isEmpty())
        {
            return ecuCalDef;
        }
        if (filename.endsWith(QString(".")))
        {
            filename.remove(filename.length() - 1, 1);
        }
        if (!filename.endsWith(QString(".xml")))
        {
            filename.append(QString(".xml"));
        }

        auto input = definitionHeaderInput(lineEditList, textEditList);
        if (!input.has_value())
        {
            log_definition_error(
                "Unable to create definition",
                input.error());
            QMessageBox::warning(
                this,
                tr("Definition file"),
                "Unable to create definition: " +
                    QString::fromStdString(input.error().detail));
            return nullptr;
        }
        for (const QLineEdit *editor : lineEditList)
        {
            if (editor->objectName() != "include")
            {
                emit LOG_D(editor->text(), true, true);
            }
        }
        fastecu::Status status =
            submit_new_definition(filename.toStdString(), *input);
        if (!status.has_value())
        {
            QMessageBox::warning(
                this,
                tr("Definition file"),
                "Unable to open definition file for writing: " +
                    QString::fromStdString(status.error().detail));
            return nullptr;
        }
        configValues->ecuflash_def_cal_id.append(
            QString::fromStdString(input->xml_id));
        configValues->ecuflash_def_cal_id_addr.append(
            lineEditValue(lineEditList, "internalidaddress"));
        configValues->ecuflash_def_ecu_id.append(
            lineEditValue(lineEditList, "ecuid"));
        configValues->ecuflash_def_filename.append(filename);
    }

    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::use_existing_definition_for_rom(FileActions::EcuCalDefStructure *ecuCalDef)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    QString filename;
    QFileDialog openDialog;
    QFileDialog saveDialog;
    bool isFileSelected = false;

    while (filename.isEmpty() && !isFileSelected)
    {
        openDialog.setDefaultSuffix("xml");
        filename = QFileDialog::getOpenFileName(this, tr("Select definition file"), configValues->ecuflash_definition_files_directory, tr("Definition file (*.xml)"));
        if (filename.isEmpty())
        {
            QDialog *definitionDialog = new QDialog(this);
            QVBoxLayout *vBoxLayout = new QVBoxLayout(definitionDialog);
            QLabel *label = new QLabel("No file selected!\n\nIf you still want to select file click 'Ok'\nIf you want to continue to use ROM without definition, click 'Cancel'");
            QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            connect(buttonBox, &QDialogButtonBox::accepted, definitionDialog, &QDialog::accept);
            connect(buttonBox, &QDialogButtonBox::rejected, definitionDialog, &QDialog::reject);

            vBoxLayout->addWidget(label);
            vBoxLayout->addWidget(buttonBox);

            int result = definitionDialog->exec();
            if (result == QDialog::Rejected)
            {
                isFileSelected = true;
            }
        }
    }
    if (filename.isEmpty())
    {
        return ecuCalDef;
    }

    const QString source = filename;
    auto sourceContents = definitionFileRepository_.read(source.toStdString());
    if (!sourceContents.has_value())
    {
        log_definition_error(
            "Unable to import definition",
            sourceContents.error());
        QMessageBox::warning(this, tr("Definition file"), "Unable to open definition file for reading");
        return nullptr;
    }
    const QByteArray sourceBytes(
        reinterpret_cast<const char *>(sourceContents->data()),
        static_cast<qsizetype>(sourceContents->size()));
    const QStringList headerData = collect_ecuflash_base_header_fields(
        *ecuCalDef,
        {QString::fromUtf8(sourceBytes)});

    QDialog *definitionDialog = new QDialog(this);
    QVBoxLayout *vBoxLayout = new QVBoxLayout(definitionDialog);
    QLabel *label = new QLabel("Please provide ROM Information:");
    vBoxLayout->addWidget(label);

    QGridLayout *defHeaderGridLayout = new QGridLayout();
    QList<QLineEdit *> lineEditList;
    QList<QTextEdit *> textEditList;
    int index = 0;
    emit LOG_D("Create header", true, true);
    for (int i = 0; i < headerData.length(); i += 2)
    {
        QLabel *label = new QLabel(ecuCalDef->DefHeaderStrings.at(index));
        defHeaderGridLayout->addWidget(label, index, 0);

        if (headerData.at(i) == "notes")
        {
            textEditList.append(new QTextEdit());
            textEditList.at(textEditList.length() - 1)->setObjectName(headerData.at(i));
            textEditList.at(textEditList.length() - 1)->setText(headerData.at(i + 1));
            defHeaderGridLayout->addWidget(textEditList.at(textEditList.length() - 1), index + 1, 0, 1, 2);
        }
        else
        {
            lineEditList.append(new QLineEdit());
            lineEditList.at(lineEditList.length() - 1)->setObjectName(headerData.at(i));
            lineEditList.at(lineEditList.length() - 1)->setText(headerData.at(i + 1));
            defHeaderGridLayout->addWidget(lineEditList.at(lineEditList.length() - 1), index, 1);
        }
        index++;
    }
    vBoxLayout->addLayout(defHeaderGridLayout);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    vBoxLayout->addWidget(buttonBox);
    connect(buttonBox, &QDialogButtonBox::accepted, definitionDialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, definitionDialog, &QDialog::reject);

    definitionDialog->setMinimumWidth(500);
    int result = definitionDialog->exec();
    if (result == QDialog::Accepted)
    {
        filename.clear();
        while (filename.isEmpty() && !isFileSelected)
        {
            saveDialog.setDefaultSuffix("xml");
            filename = QFileDialog::getSaveFileName(this, tr("Select definition file"), configValues->ecuflash_definition_files_directory, tr("Definition file (*.xml)"));
            if (filename.isEmpty())
            {
                QDialog *definitionDialog = new QDialog(this);
                QVBoxLayout *vBoxLayout = new QVBoxLayout(definitionDialog);
                QLabel *label = new QLabel("No file selected!\n\nIf you still want to create file click 'Ok'\nIf you want to continue to use ROM without definition, click 'Cancel'");
                QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
                connect(buttonBox, &QDialogButtonBox::accepted, definitionDialog, &QDialog::accept);
                connect(buttonBox, &QDialogButtonBox::rejected, definitionDialog, &QDialog::reject);

                vBoxLayout->addWidget(label);
                vBoxLayout->addWidget(buttonBox);

                int result = definitionDialog->exec();
                if (result == QDialog::Rejected)
                {
                    isFileSelected = true;
                }
            }
        }
        if (filename.isEmpty())
        {
            return ecuCalDef;
        }
        if (filename.endsWith(QString(".")))
        {
            filename.remove(filename.length() - 1, 1);
        }
        if (!filename.endsWith(QString(".xml")))
        {
            filename.append(QString(".xml"));
        }

        auto input = definitionHeaderInput(lineEditList, textEditList);
        if (!input.has_value())
        {
            log_definition_error(
                "Unable to import definition",
                input.error());
            QMessageBox::warning(
                this,
                tr("Definition file"),
                "Unable to import definition: " +
                    QString::fromStdString(input.error().detail));
            return nullptr;
        }
        emit LOG_D("Write to file", true, true);
        for (const QLineEdit *editor : lineEditList)
        {
            if (editor->objectName() != "include")
            {
                emit LOG_D(editor->text(), true, true);
            }
        }
        fastecu::Status status = submit_imported_definition(
            source.toStdString(),
            filename.toStdString(),
            *input);
        if (!status.has_value())
        {
            QMessageBox::warning(
                this,
                tr("Definition file"),
                "Unable to open definition file for writing: " +
                    QString::fromStdString(status.error().detail));
            return nullptr;
        }
        configValues->ecuflash_def_cal_id.append(
            QString::fromStdString(input->xml_id));
        configValues->ecuflash_def_cal_id_addr.append(
            lineEditValue(lineEditList, "internalidaddress"));
        configValues->ecuflash_def_ecu_id.append(
            lineEditValue(lineEditList, "ecuid"));
        configValues->ecuflash_def_filename.append(filename);
    }

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

FileActions::EcuCalDefStructure *FileActions::open_subaru_rom_file(FileActions::EcuCalDefStructure *ecuCalDef, QString filename)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    bool id_is_ascii = false;
    bool bStatus = false;

    fastecu::Status opened = calibrationAdapter_.open_rom_bytes(*ecuCalDef, filename, *configValues);
    if (!opened.has_value())
    {
        log_definition_error("Unable to open calibration file", opened.error());
        QMessageBox::warning(this, tr("Calibration file"), "Unable to open calibration file for reading");
        return nullptr;
    }
    filename = ecuCalDef->FullFileName;

    ecuCalDef->use_ecuflash_definition = false;
    ecuCalDef->use_romraider_definition = false;

    if ((configValues->primary_definition_base == "ecuflash" || configValues->use_romraider_definitions != "enabled") && configValues->ecuflash_definition_files_directory.length())
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

    QByteArray padding;
    padding.clear();
    if (ecuCalDef->RomInfo.at(FlashMethod).startsWith("sub_ecu_denso_mc68hc16y5_02") && ecuCalDef->FileSize.toUInt() < 190 * 1024)
    {
        for (int i = 0; i < 0x8000; i++)
        {
            ecuCalDef->FullRomData.insert(0x20000, (uint8_t)0xff);
        }
    }
    // emit LOG_D("QByteArray size = " + ecuCalDef->FullRomData.length(), true, true);

    // Deliberately after the padding block above, not before it: padding
    // grows FullRomData by 0x8000 bytes, and a definition authored against
    // the padded image would be rejected by a check run against the
    // pre-padded length.
    if (ecuCalDef->use_romraider_definition || ecuCalDef->use_ecuflash_definition)
    {
        const fastecu::definition::DefinitionFormat matchedFormat =
            ecuCalDef->use_ecuflash_definition
                ? fastecu::definition::DefinitionFormat::EcuFlash
                : fastecu::definition::DefinitionFormat::RomRaider;
        const fastecu::definition::RomDefinition *romDefinition =
            resolved_definition(matchedFormat, ecuCalDef->RomId);
        if (romDefinition == nullptr)
        {
            // A skipped size check must never be silent: the map-data loops
            // below are exactly what it fences.
            emit LOG_W(
                "ROM size validation skipped: no resolved definition for id " +
                    ecuCalDef->RomId,
                true,
                true);
        }
        else
        {
            fastecu::Status sizeOk = fastecu::calibration::validate_rom_size(
                *romDefinition, static_cast<std::size_t>(ecuCalDef->FullRomData.length()));
            if (!sizeOk.has_value())
            {
                log_definition_error("Error in expected ROM size", sizeOk.error());
                QMessageBox::warning(this, tr("File size error"), "Error in expected ROM size!");
                ecuCalDef->NameList.clear();
                return ecuCalDef;
            }
        }
    }

    int storagesize = 0;
    QString storagetype = 0;
    QString mapData;

    int32_t signedDataByte = 0;
    uint32_t dataByte = 0;
    uint32_t startPos = 0;
    uint32_t interval = 0;
    uint32_t byteAddress = 0;

    union mapData
    {
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_alue[4];
        uint16_t word_alue[2];
        uint32_t dword_value;
        float float_value;
    } mapDataValue{};

    for (int i = 0; i < ecuCalDef->NameList.length(); i++)
    {
        storagesize = 1;
        storagetype = ecuCalDef->StorageTypeList.at(i);
        if (storagetype == "uint16" || storagetype == "int16")
        {
            storagesize = 2;
        }
        else if (storagetype == "uint24" || storagetype == "int24")
        {
            storagesize = 3;
        }
        else if (storagetype == "uint32" || storagetype == "int32" || storagetype == "float")
        {
            storagesize = 4;
        }
        mapData.clear();
        if (ecuCalDef->StorageTypeList.at(i) == "bloblist")
        {
            storagesize = ecuCalDef->SelectionsValueList.at(i).split(",").at(0).length() / 2;
            uint8_t dataByte = 0;
            uint32_t byteAddress = ecuCalDef->AddressList.at(i).toUInt(&bStatus, 16);
            for (int k = 0; k < storagesize; k++)
            {
                dataByte = (uint8_t)ecuCalDef->FullRomData.at(byteAddress + k);
                mapData.append(QString("%1").arg(dataByte, 2, 16, QLatin1Char('0')));
            }
            ecuCalDef->MapData.replace(i, mapData);
        }
        else
        {
            for (unsigned j = 0; j < ecuCalDef->XSizeList.at(i).toUInt() * ecuCalDef->YSizeList.at(i).toUInt(); j++)
            {
                signedDataByte = 0;
                dataByte = 0;
                startPos = ecuCalDef->StartPosList.at(i).toUInt(&bStatus, 16);
                interval = ecuCalDef->IntervalList.at(i).toUInt(&bStatus, 16);
                byteAddress = ecuCalDef->AddressList.at(i).toUInt(&bStatus, 16) + (j * storagesize * interval + (startPos - 1) * storagesize);
                mapDataValue.dword_value = 0;

                if (ecuCalDef->RomInfo.at(FlashMethod) == "wrx02" && (uint32_t)ecuCalDef->FullRomData.length() < byteAddress)
                {
                    byteAddress -= 0x8000;
                }
                for (int k = 0; k < storagesize; k++)
                {
                    if (ecuCalDef->EndianList.at(i) == "little" || ecuCalDef->StorageTypeList.at(i) == "float")
                    {
                        mapDataValue.byte_alue[k] = (uint8_t)ecuCalDef->FullRomData.at(byteAddress + storagesize - 1 - k);
                    }
                    else
                    {
                        if (storagetype.startsWith("uint"))
                        {
                            dataByte = (dataByte << 8) + (uint8_t)ecuCalDef->FullRomData.at(byteAddress + k);
                        }
                        else
                        {
                            signedDataByte = (signedDataByte << 8) + ecuCalDef->FullRomData.at(byteAddress + k);
                        }
                    }
                }
                double value = 0;
                if (ecuCalDef->TypeList.at(i) != "Selectable")
                {
                    if (ecuCalDef->StorageTypeList.at(i) == "float")
                    {
                        value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->FromByteList.at(i), QString::number(mapDataValue.float_value, 'g', float_precision)));
                    }
                    else
                    {
                        if (storagetype.startsWith("uint"))
                        {
                            value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->FromByteList.at(i), QString::number(dataByte)));
                        }
                        else
                        {
                            value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->FromByteList.at(i), QString::number(signedDataByte)));
                        }
                    }
                }
                mapData.append(QString::number(value, 'g', float_precision) + ",");
            }
            ecuCalDef->MapData.replace(i, mapData);

            if (ecuCalDef->XSizeList.at(i).toUInt() > 1)
            {
                if (ecuCalDef->XScaleTypeList.at(i) == "Static Y Axis" || ecuCalDef->XScaleTypeList.at(i) == "Static X Axis")
                {
                    ecuCalDef->XScaleData.replace(i, ecuCalDef->XScaleStaticDataList.at(i));
                }
                else if (ecuCalDef->XScaleTypeList.at(i) == "X Axis" || (ecuCalDef->XScaleTypeList.at(i) == "Y Axis" && ecuCalDef->TypeList.at(i) == "2D"))
                {
                    storagesize = 1;
                    storagetype = ecuCalDef->XScaleStorageTypeList.at(i);
                    if (storagetype == "uint16" || storagetype == "int16")
                    {
                        storagesize = 2;
                    }
                    else if (storagetype == "uint24" || storagetype == "int24")
                    {
                        storagesize = 3;
                    }
                    else if (storagetype == "uint32" || storagetype == "int32" || storagetype == "float")
                    {
                        storagesize = 4;
                    }
                    mapData.clear();
                    for (unsigned j = 0; j < ecuCalDef->XSizeList.at(i).toUInt(); j++)
                    {
                        dataByte = 0;
                        startPos = ecuCalDef->XScaleStartPosList.at(i).toUInt(&bStatus, 16);
                        interval = ecuCalDef->XScaleIntervalList.at(i).toUInt(&bStatus, 16);
                        byteAddress = ecuCalDef->XScaleAddressList.at(i).toUInt(&bStatus, 16) + (j * storagesize * interval + (startPos - 1) * storagesize);
                        mapDataValue.dword_value = 0;

                        if (ecuCalDef->RomInfo.at(FlashMethod) == "wrx02" && (uint32_t)ecuCalDef->FullRomData.length() < byteAddress)
                        {
                            byteAddress -= 0x8000;
                        }

                        for (int k = 0; k < storagesize; k++)
                        {
                            if (ecuCalDef->XScaleEndianList.at(i) == "little" || ecuCalDef->XScaleStorageTypeList.at(i) == "float")
                            {
                                mapDataValue.byte_alue[k] = (uint8_t)ecuCalDef->FullRomData.at(byteAddress + storagesize - 1 - k);
                            }
                            else
                            {
                                if (storagetype.startsWith("uint"))
                                {
                                    dataByte = (dataByte << 8) + (uint8_t)ecuCalDef->FullRomData.at(byteAddress + k);
                                }
                                else
                                {
                                    signedDataByte = (signedDataByte << 8) + ecuCalDef->FullRomData.at(byteAddress + k);
                                }
                            }
                        }
                        double value = 0;
                        if (ecuCalDef->XScaleTypeList.at(i) != "Selectable")
                        {
                            if (ecuCalDef->XScaleStorageTypeList.at(i) == "float")
                            {
                                value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->XScaleFromByteList.at(i), QString::number(mapDataValue.float_value, 'g', float_precision)));
                            }
                            else
                            {
                                if (storagetype.startsWith("uint"))
                                {
                                    value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->XScaleFromByteList.at(i), QString::number(dataByte)));
                                }
                                else
                                {
                                    value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->XScaleFromByteList.at(i), QString::number(signedDataByte)));
                                }
                            }
                        }
                        mapData.append(QString::number(value, 'g', float_precision) + ",");
                    }
                    ecuCalDef->XScaleData.replace(i, mapData);
                }
            }
            else
            {
                ecuCalDef->XScaleData.replace(i, " ");
            }
            if (ecuCalDef->YSizeList.at(i).toUInt() > 1)
            {
                storagesize = 1;
                storagetype = ecuCalDef->YScaleStorageTypeList.at(i);
                if (storagetype == "uint16" || storagetype == "int16")
                {
                    storagesize = 2;
                }
                else if (storagetype == "uint24" || storagetype == "int24")
                {
                    storagesize = 3;
                }
                else if (storagetype == "uint32" || storagetype == "int32" || storagetype == "float")
                {
                    storagesize = 4;
                }
                mapData.clear();
                for (unsigned j = 0; j < ecuCalDef->YSizeList.at(i).toUInt(); j++)
                {
                    dataByte = 0;
                    startPos = ecuCalDef->YScaleStartPosList.at(i).toUInt(&bStatus, 16);
                    interval = ecuCalDef->YScaleIntervalList.at(i).toUInt(&bStatus, 16);
                    byteAddress = ecuCalDef->YScaleAddressList.at(i).toUInt(&bStatus, 16) + (j * storagesize * interval + (startPos - 1) * storagesize);
                    mapDataValue.dword_value = 0;

                    if (ecuCalDef->RomInfo.at(FlashMethod) == "wrx02" && (uint32_t)ecuCalDef->FullRomData.length() < byteAddress)
                    {
                        byteAddress -= 0x8000;
                    }
                    for (int k = 0; k < storagesize; k++)
                    {
                        if (ecuCalDef->YScaleEndianList.at(i) == "little" || ecuCalDef->YScaleStorageTypeList.at(i) == "float")
                        {
                            mapDataValue.byte_alue[k] = (uint8_t)ecuCalDef->FullRomData.at(byteAddress + storagesize - 1 - k);
                        }
                        else
                        {
                            if (storagetype.startsWith("uint"))
                            {
                                dataByte = (dataByte << 8) + (uint8_t)ecuCalDef->FullRomData.at(byteAddress + k);
                            }
                            else
                            {
                                signedDataByte = (signedDataByte << 8) + ecuCalDef->FullRomData.at(byteAddress + k);
                            }
                        }
                    }
                    double value = 0;
                    if (ecuCalDef->YScaleTypeList.at(i) != "Selectable")
                    {
                        if (ecuCalDef->YScaleStorageTypeList.at(i) == "float")
                        {
                            value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->YScaleFromByteList.at(i), QString::number(mapDataValue.float_value, 'g', float_precision)));
                        }
                        else
                        {
                            if (storagetype.startsWith("uint"))
                            {
                                value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->YScaleFromByteList.at(i), QString::number(dataByte)));
                            }
                            else
                            {
                                value = calculate_value_from_expression(parse_stringlist_from_expression_string(ecuCalDef->YScaleFromByteList.at(i), QString::number(signedDataByte)));
                            }
                        }
                    }
                    mapData.append(QString::number(value, 'g', float_precision) + ",");
                }
                ecuCalDef->YScaleData.replace(i, mapData);
            }
            else
            {
                ecuCalDef->YScaleData.replace(i, " ");
            }
        }
    }

    if (ecuCalDef == nullptr)
    {
        QMessageBox::warning(this, tr("Calibration file"), QString("Unable to find definition for selected calibration file with ECU ID: ") + ".");
        return nullptr;
    }

    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::save_subaru_rom_file(FileActions::EcuCalDefStructure *ecuCalDef, const QString& filename)
{
    EcuCalDefStructure *saved = calibrationAdapter_.save_subaru_rom_file(ecuCalDef, filename);
    if (saved == nullptr)
    {
        // A failed write is the one failure in this file that must never be
        // silent: both callers in MainWindow historically ignored the return
        // value, so this dialog (and the log line beside it) is the user's
        // only signal that the ROM they are about to flash was not written.
        emit LOG_E("Unable to open file " + filename + " for writing", true, true);
        QMessageBox::warning(this, tr("Ecu calibration file"),
                             "Unable to open file " + filename + " for writing");
    }
    return saved;
}

FileActions::EcuCalDefStructure *FileActions::checksum_correction(FileActions::EcuCalDefStructure *ecuCalDef)
{
    const fastecu::checksum::ChecksumSelection selection{
        .make = ConfigValuesStruct.flash_protocol_selected_make.toStdString(),
        .checksum_flag = ConfigValuesStruct.flash_protocol_selected_checksum.toStdString(),
        .flash_method = ConfigValuesStruct.flash_protocol_selected_protocol_name.toStdString(),
        .mcu_type = ecuCalDef->McuType.toStdString(),
        .rom_id = ecuCalDef->RomId.toStdString(),
    };

    emit LOG_D("Protocol: " + ConfigValuesStruct.flash_protocol_selected_protocol_name, true, true);
    emit LOG_D("Make: " + ConfigValuesStruct.flash_protocol_selected_make, true, true);
    emit LOG_D("Checksum: " + ConfigValuesStruct.flash_protocol_selected_checksum, true, true);

    const flashdev_t *device = fastecu::checksum::find_flash_device(selection.mcu_type);
    if (device == nullptr)
    {
        emit LOG_E("Unknown MCU type: " + ecuCalDef->McuType, true, true);
        return ecuCalDef;
    }
    emit LOG_D("ecuCalDef->McuType: " + ecuCalDef->McuType + " " + ConfigValuesStruct.flash_protocol_selected_mcu, true, true);
    emit LOG_D("Size: 0x" + QString::number(ecuCalDef->FullRomData.length(), 16) + " -> 0x" +
                   QString::number(device->romsize, 16),
               true, true);

    const fastecu::checksum::LegacyChecksumAdapterResult result = checksumAdapter_.checksum_correction(
        bytes::view(ecuCalDef->FullRomData), ecuCalDef->use_romraider_definition, ecuCalDef->use_ecuflash_definition,
        selection, this);

    if (result.canceled_due_to_missing_module)
    {
        emit LOG_D("Checksum calculation canceled!", true, true);
    }
    if (result.corrected_rom_data.has_value())
    {
        ecuCalDef->FullRomData = bytes::toQByteArray(bytes::ByteView(*result.corrected_rom_data));
    }
    return ecuCalDef;
}

QStringList FileActions::parse_stringlist_from_expression_string(const QString& expression, const QString& x)
{
    return ExpressionEvaluator::parse(expression, x);
}

double FileActions::calculate_value_from_expression(const QStringList& expression)
{
    return ExpressionEvaluator::evaluate(expression, float_precision);
}

QString FileActions::parse_nrc_message(const QByteArray& nrc)
{
    return NrcParser::parse(nrc, neg_rsp_codes);
}

QString FileActions::parse_dtc_message(uint16_t dtc)
{
    return DtcParser::parse(dtc, dtc_Pxxxx_codes, dtc_Cxxxx_codes,
                            dtc_Bxxxx_codes, dtc_Uxxxx_codes);
}
