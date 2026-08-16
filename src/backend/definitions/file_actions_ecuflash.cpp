#include "src/backend/definitions/file_actions.h"

// Qt compatibility wrappers over the portable definition service.

#include <set>

FileActions::ConfigValuesStructure *FileActions::create_ecuflash_def_id_list(ConfigValuesStructure *configValues)
{
    if (configValues->ecuflash_definition_files_directory.isEmpty())
    {
        emit LOG_D("No EcuFlash definition files directory", true, true);
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
    emit LOG_D(QString::number(sources.size()) + " EcuFlash definition files found", true, true);
    emit LOG_D(QString::number(configValues->ecuflash_def_cal_id.size()) + " EcuFlash ecu id's found", true, true);
    return configValues;
}

FileActions::EcuCalDefStructure *FileActions::read_ecuflash_ecu_def(EcuCalDefStructure *ecuCalDef,
                                                                    const QString& cal_id)
{
    if (ConfigValuesStruct.ecuflash_def_cal_id.isEmpty())
    {
        return nullptr;
    }
    emit LOG_D("Search ID: " + cal_id, true, true);
    if (!ConfigValuesStruct.ecuflash_def_cal_id.contains(cal_id))
    {
        return ecuCalDef;
    }

    const QString source = definition_source(fastecu::definition::DefinitionFormat::EcuFlash, cal_id);
    emit LOG_D("EcuFlash ID found: " + cal_id + " " + cal_id, true, true);
    emit LOG_D("EcuFlash def file name: " + source, true, true);

    const fastecu::Status replaced =
        load_configured_definition(*ecuCalDef, fastecu::definition::DefinitionFormat::EcuFlash, cal_id);
    if (!replaced.has_value())
    {
        const bool missing =
            log_definition_load_failure("Unable to read EcuFlash definition " + cal_id, replaced.error(), source,
                                        tr("Ecu definitions file"), "Unable to open ECU definition file ");
        if (missing)
        {
            return nullptr;
        }
        return ecuCalDef;
    }
    emit LOG_D("Found ID: " + cal_id, true, true);
    emit LOG_D("Definition for CAL ID " + cal_id + " succesfully read, start parsing definition scalings...", true,
               true);
    return ecuCalDef;
}
