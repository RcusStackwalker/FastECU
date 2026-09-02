#include "src/backend/definitions/file_actions.h"

// Qt compatibility wrappers over the portable definition service.

#include <string>
#include <vector>

FileActions::ConfigValuesStructure *FileActions::create_romraider_def_id_list(ConfigValuesStructure *configValues)
{
    if (configValues->romraider_definition_files.isEmpty())
    {
        events_.log(fastecu::LogLevel::Debug, "No RomRaider definition files");
        return configValues;
    }

    std::vector<std::string> handles;
    handles.reserve(static_cast<std::size_t>(configValues->romraider_definition_files.size()));
    for (const QString& handle : configValues->romraider_definition_files)
    {
        events_.log(fastecu::LogLevel::Debug,
                    std::format("Reading RomRaider ID's from file: {}", handle.toStdString()));
        handles.push_back(handle.toStdString());
    }

    const fastecu::Status replaced = definitionAdapter_.replace_romraider_catalog(*configValues, handles);
    if (!replaced.has_value())
    {
        log_definition_error("Unable to build RomRaider definition catalog", replaced.error());
        for (const QString& handle : configValues->romraider_definition_files)
        {
            if (!definitionFileSystem_.exists(handle.toStdString()))
            {
                events_.notice(
                    std::format("Ecu definition file: Unable to open romraider definition file {} for reading",
                                handle.toStdString()));
                break;
            }
        }
        return configValues;
    }

    strip_legacy_address_prefixes(configValues->romraider_def_cal_id_addr);
    events_.log(fastecu::LogLevel::Debug,
                std::format("{} RomRaider definition files found", configValues->romraider_definition_files.size()));
    events_.log(fastecu::LogLevel::Debug,
                std::format("{} RomRaider ecu id's found", configValues->romraider_def_cal_id.size()));
    return configValues;
}

FileActions::EcuCalDefStructure *FileActions::read_romraider_ecu_base_def(EcuCalDefStructure *ecuCalDef)
{
    const QString source = ecuCalDef->DefinitionFileName;
    if (source.isEmpty())
    {
        log_definition_error("Unable to read RomRaider base definition", fastecu::Error{
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
        log_definition_error("Unable to read RomRaider base definition", fastecu::Error{
                                                                             fastecu::ErrorKind::InvalidConfig,
                                                                             "definition ID is missing",
                                                                         });
        return nullptr;
    }

    // This is a single, specifically-named required file (the base this ROM's identity
    // points at), not a directory scan -- if it can't be read or parsed, that failure must be
    // reported directly rather than silently treated as an empty catalog.
    const std::vector<std::string> handles{source.toStdString()};
    auto catalog = definitionService_.build_romraider_catalog(handles, /*skip_unusable_handles=*/false);
    if (!catalog.has_value())
    {
        log_definition_error("Unable to read RomRaider base definition", catalog.error());
        if (!definitionFileSystem_.exists(source.toStdString()))
        {
            events_.notice(
                std::format("Ecu definitions file: Unable to open OEM ecu base definitions file {} for reading",
                            source.toStdString()));
        }
        return nullptr;
    }

    const fastecu::Status replaced = definitionAdapter_.replace_definition(
        *ecuCalDef, *catalog, fastecu::definition::DefinitionFormat::RomRaider, definitionId.toStdString());
    if (!replaced.has_value())
    {
        log_definition_error("Unable to read RomRaider base definition", replaced.error());
        return nullptr;
    }
    normalize_definition_addresses(*ecuCalDef);
    apply_flash_method_alias(*ecuCalDef);
    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::read_romraider_ecu_def(EcuCalDefStructure *ecuCalDef,
                                                                     const QString& cal_id)
{
    if (ConfigValuesStruct.romraider_definition_files.isEmpty() &&
        ConfigValuesStruct.ecuflash_definition_files_directory.isEmpty())
    {
        events_.notice("Ecu definition file: No RomRaider definition file(s), use definition manager at "
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

    const QString source = definition_source(fastecu::definition::DefinitionFormat::RomRaider, cal_id);
    const fastecu::Status replaced =
        load_configured_definition(*ecuCalDef, fastecu::definition::DefinitionFormat::RomRaider, cal_id);
    if (!replaced.has_value())
    {
        const bool missing =
            log_definition_load_failure("Unable to read RomRaider definition " + cal_id, replaced.error(), source,
                                        "Ecu definitions file", "Unable to open ECU definition file ");
        if (missing)
        {
            return nullptr;
        }
        return ecuCalDef;
    }
    events_.log(fastecu::LogLevel::Debug, std::format("XML ID: {} {}", cal_id.toStdString(), cal_id.toStdString()));
    return ecuCalDef;
}
