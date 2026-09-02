#include "src/backend/definitions/file_actions.h"

// Qt compatibility wrappers over the portable definition service.

#include <set>

FileActions::ConfigValuesStructure *FileActions::create_ecuflash_def_id_list(ConfigValuesStructure *configValues)
{
    if (configValues->ecuflash_definition_files_directory.isEmpty())
    {
        events_.log(fastecu::LogLevel::Debug, "No EcuFlash definition files directory");
        return configValues;
    }

    const fastecu::Status replaced = definitionAdapter_.replace_ecuflash_catalog(
        *configValues, configValues->ecuflash_definition_files_directory.toStdString(), submittedEcuflashHandles_);
    if (!replaced.has_value())
    {
        log_definition_error("Unable to build EcuFlash definition catalog", replaced.error());
        return configValues;
    }

    strip_legacy_address_prefixes(configValues->ecuflash_def_cal_id_addr);
    std::set<QString> sources;
    for (const QString& source : configValues->ecuflash_def_filename)
    {
        sources.insert(source);
    }
    events_.log(fastecu::LogLevel::Debug, std::format("{} EcuFlash definition files found", sources.size()));
    events_.log(fastecu::LogLevel::Debug,
                std::format("{} EcuFlash ecu id's found", configValues->ecuflash_def_cal_id.size()));
    return configValues;
}

FileActions::EcuCalDefStructure *FileActions::read_ecuflash_ecu_def(EcuCalDefStructure *ecuCalDef,
                                                                    const QString& cal_id)
{
    if (ConfigValuesStruct.ecuflash_def_cal_id.isEmpty())
    {
        return nullptr;
    }
    events_.log(fastecu::LogLevel::Debug, std::format("Search ID: {}", cal_id.toStdString()));
    if (!ConfigValuesStruct.ecuflash_def_cal_id.contains(cal_id))
    {
        return ecuCalDef;
    }

    const QString source = definition_source(fastecu::definition::DefinitionFormat::EcuFlash, cal_id);
    events_.log(fastecu::LogLevel::Debug,
                std::format("EcuFlash ID found: {} {}", cal_id.toStdString(), cal_id.toStdString()));
    events_.log(fastecu::LogLevel::Debug, std::format("EcuFlash def file name: {}", source.toStdString()));

    const fastecu::Status replaced =
        load_configured_definition(*ecuCalDef, fastecu::definition::DefinitionFormat::EcuFlash, cal_id);
    if (!replaced.has_value())
    {
        const bool missing =
            log_definition_load_failure("Unable to read EcuFlash definition " + cal_id, replaced.error(), source,
                                        "Ecu definitions file", "Unable to open ECU definition file ");
        if (missing)
        {
            return nullptr;
        }
        return ecuCalDef;
    }
    events_.log(fastecu::LogLevel::Debug, std::format("Found ID: {}", cal_id.toStdString()));
    events_.log(fastecu::LogLevel::Debug,
                std::format("Definition for CAL ID {} succesfully read, start parsing definition scalings...",
                            cal_id.toStdString()));
    return ecuCalDef;
}
