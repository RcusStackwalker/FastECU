#include "src/backend/definitions/file_actions.h"

// Qt compatibility wrappers over the portable definition service.

#include <string>
#include <vector>

FileActions::ConfigValuesStructure *FileActions::create_romraider_def_id_list(
    ConfigValuesStructure *configValues)
{
    if (configValues->romraider_definition_files.isEmpty())
    {
        emit LOG_D("No RomRaider definition files", true, true);
        return configValues;
    }

    std::vector<std::string> handles;
    handles.reserve(static_cast<std::size_t>(
        configValues->romraider_definition_files.size()));
    for (const QString& handle : configValues->romraider_definition_files)
    {
        emit LOG_D(
            "Reading RomRaider ID's from file: " + handle,
            true,
            true);
        handles.push_back(handle.toStdString());
    }

    const fastecu::Status replaced =
        definitionAdapter_.replace_romraider_catalog(*configValues, handles);
    if (!replaced.has_value())
    {
        log_definition_error(
            "Unable to build RomRaider definition catalog",
            replaced.error());
        for (const QString& handle :
             configValues->romraider_definition_files)
        {
            if (!definitionFileSystem_.exists(handle.toStdString()))
            {
                QMessageBox::warning(
                    this,
                    tr("Ecu definition file"),
                    "Unable to open romraider definition file " + handle +
                        " for reading");
                break;
            }
        }
        return configValues;
    }

    for (QString& address : configValues->romraider_def_cal_id_addr)
    {
        if (address.startsWith("0x"))
        {
            address.remove(0, 2);
        }
    }
    emit LOG_D(
        QString::number(configValues->romraider_definition_files.size()) +
            " RomRaider definition files found",
        true,
        true);
    emit LOG_D(
        QString::number(configValues->romraider_def_cal_id.size()) +
            " RomRaider ecu id's found",
        true,
        true);
    return configValues;
}

FileActions::EcuCalDefStructure *FileActions::read_romraider_ecu_base_def(
    EcuCalDefStructure *ecuCalDef)
{
    const QString source = ecuCalDef->DefinitionFileName;
    if (source.isEmpty())
    {
        log_definition_error(
            "Unable to read RomRaider base definition",
            fastecu::Error{
                fastecu::ErrorKind::InvalidConfig,
                "definition source is missing",
            });
        return nullptr;
    }

    QString definitionId;
    if (ecuCalDef->RomInfo.size() > XmlId)
    {
        definitionId = ecuCalDef->RomInfo.at(XmlId).trimmed();
    }
    if (definitionId.isEmpty() && ecuCalDef->RomInfo.size() > RomBase)
    {
        definitionId = ecuCalDef->RomInfo.at(RomBase).trimmed();
    }
    if (definitionId.isEmpty())
    {
        log_definition_error(
            "Unable to read RomRaider base definition",
            fastecu::Error{
                fastecu::ErrorKind::InvalidConfig,
                "definition ID is missing",
            });
        return nullptr;
    }

    const std::vector<std::string> handles{source.toStdString()};
    auto catalog = definitionService_.build_romraider_catalog(handles);
    if (!catalog)
    {
        log_definition_error(
            "Unable to read RomRaider base definition",
            catalog.error());
        if (!definitionFileSystem_.exists(source.toStdString()))
        {
            QMessageBox::warning(
                this,
                tr("Ecu definitions file"),
                "Unable to open OEM ecu base definitions file " + source +
                    " for reading");
        }
        return nullptr;
    }

    const fastecu::Status replaced = definitionAdapter_.replace_definition(
        *ecuCalDef,
        *catalog,
        fastecu::definition::DefinitionFormat::RomRaider,
        definitionId.toStdString());
    if (!replaced.has_value())
    {
        log_definition_error(
            "Unable to read RomRaider base definition",
            replaced.error());
        return nullptr;
    }
    normalize_definition_addresses(*ecuCalDef);
    apply_flash_method_alias(*ecuCalDef);
    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::read_romraider_ecu_def(
    EcuCalDefStructure *ecuCalDef,
    const QString& cal_id)
{
    if (ConfigValuesStruct.romraider_definition_files.isEmpty() &&
        ConfigValuesStruct.ecuflash_definition_files_directory.isEmpty())
    {
        QMessageBox::warning(
            this,
            tr("Ecu definition file"),
            "No RomRaider definition file(s), use definition manager at "
            "'Edit' menu to choose file(s)");
        return nullptr;
    }
    if (ConfigValuesStruct.romraider_def_cal_id.isEmpty())
    {
        return nullptr;
    }
    if (!ConfigValuesStruct.romraider_def_cal_id.contains(cal_id))
    {
        return ecuCalDef;
    }

    const QString source = definition_source(
        fastecu::definition::DefinitionFormat::RomRaider,
        cal_id);
    auto catalog =
        build_definition_catalog(
            fastecu::definition::DefinitionFormat::RomRaider);
    if (!catalog)
    {
        log_definition_error(
            "Unable to read RomRaider definition " + cal_id,
            catalog.error());
        if (!source.isEmpty() &&
            !definitionFileSystem_.exists(source.toStdString()))
        {
            QMessageBox::warning(
                this,
                tr("Ecu definitions file"),
                "Unable to open ECU definition file " + source +
                    " for reading");
            return nullptr;
        }
        return ecuCalDef;
    }

    const fastecu::Status replaced = definitionAdapter_.replace_definition(
        *ecuCalDef,
        *catalog,
        fastecu::definition::DefinitionFormat::RomRaider,
        cal_id.toStdString());
    if (!replaced.has_value())
    {
        log_definition_error(
            "Unable to read RomRaider definition " + cal_id,
            replaced.error());
        if (!source.isEmpty() &&
            !definitionFileSystem_.exists(source.toStdString()))
        {
            QMessageBox::warning(
                this,
                tr("Ecu definitions file"),
                "Unable to open ECU definition file " + source +
                    " for reading");
            return nullptr;
        }
        return ecuCalDef;
    }

    normalize_definition_addresses(*ecuCalDef);
    apply_flash_method_alias(*ecuCalDef);
    emit LOG_D("XML ID: " + cal_id + " " + cal_id, true, true);
    return ecuCalDef;
}
