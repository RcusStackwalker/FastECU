#include "src/backend/definitions/file_actions.h"
#include "src/backend/definitions/legacy_definition_columns.h"

// Qt compatibility wrappers over the portable definition service.

#include <algorithm>
#include <array>
#include <set>
#include <string>

namespace
{

QString legacyScalingFormat(const QString& value_format)
{
    const bool canonicalDecimal =
        value_format.size() > 2 &&
        value_format.startsWith("0.") &&
        std::all_of(
            value_format.cbegin() + 2,
            value_format.cend(),
            [](QChar character)
            {
                return character == '0';
            });
    if (value_format == "0" || canonicalDecimal)
    {
        return value_format;
    }
    const QStringList format_text = value_format.split(".");
    QString decimal_count;
    QString decimals = "0";
    if (format_text.length() > 1 && format_text.at(1).contains("f"))
    {
        decimal_count = format_text.at(1).split("f").at(0);
    }
    if (decimal_count.toInt() > 0)
    {
        decimals.append(".");
    }
    for (int index = 0; index < decimal_count.toInt(); ++index)
    {
        decimals.append("0");
    }
    return decimals;
}

} // namespace

FileActions::ConfigValuesStructure *FileActions::create_ecuflash_def_id_list(
    ConfigValuesStructure *configValues)
{
    if (configValues->ecuflash_definition_files_directory.isEmpty())
    {
        emit LOG_D("No EcuFlash definition files directory", true, true);
        return configValues;
    }

    const fastecu::Status replaced =
        definitionAdapter_.replace_ecuflash_catalog(
            *configValues,
            configValues->ecuflash_definition_files_directory.toStdString(),
            submittedEcuflashHandles_);
    if (!replaced.has_value())
    {
        log_definition_error(
            "Unable to build EcuFlash definition catalog",
            replaced.error());
        return configValues;
    }

    strip_legacy_address_prefixes(
        configValues->ecuflash_def_cal_id_addr);
    std::set<QString> sources;
    for (const QString& source : configValues->ecuflash_def_filename)
    {
        sources.insert(source);
    }
    emit LOG_D(
        QString::number(sources.size()) +
            " EcuFlash definition files found",
        true,
        true);
    emit LOG_D(
        QString::number(configValues->ecuflash_def_cal_id.size()) +
            " EcuFlash ecu id's found",
        true,
        true);
    return configValues;
}

FileActions::EcuCalDefStructure *FileActions::read_ecuflash_ecu_def(
    EcuCalDefStructure *ecuCalDef,
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

    const QString source = definition_source(
        fastecu::definition::DefinitionFormat::EcuFlash,
        cal_id);
    emit LOG_D("EcuFlash ID found: " + cal_id + " " + cal_id, true, true);
    emit LOG_D("EcuFlash def file name: " + source, true, true);

    const fastecu::Status replaced = load_configured_definition(
        *ecuCalDef,
        fastecu::definition::DefinitionFormat::EcuFlash,
        cal_id);
    if (!replaced.has_value())
    {
        const bool missing = log_definition_load_failure(
            "Unable to read EcuFlash definition " + cal_id,
            replaced.error(),
            source,
            tr("Ecu definitions file"),
            "Unable to open ECU definition file ");
        if (missing)
        {
            return nullptr;
        }
        return ecuCalDef;
    }
    emit LOG_D("Found ID: " + cal_id, true, true);
    emit LOG_D(
        "Definition for CAL ID " + cal_id +
            " succesfully read, start parsing definition scalings...",
        true,
        true);
    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::parse_ecuflash_def_scalings(
    EcuCalDefStructure *ecuCalDef)
{
    if (ecuCalDef->use_ecuflash_definition)
    {
        return ecuCalDef;
    }

    const auto apply_scaling_columns =
        [ecuCalDef](
            qsizetype map_index,
            qsizetype scaling_index,
            const fastecu::definitions::legacy_columns::ScalingDestination& destination)
    {
        destination.storage->replace(
            map_index, ecuCalDef->ScalingStorageTypeList.at(scaling_index));
        destination.units->replace(
            map_index, ecuCalDef->ScalingUnitsList.at(scaling_index));
        destination.fine->replace(
            map_index, ecuCalDef->ScalingFineIncList.at(scaling_index));
        destination.coarse->replace(
            map_index, ecuCalDef->ScalingCoarseIncList.at(scaling_index));
        destination.minimum->replace(
            map_index, ecuCalDef->ScalingMinValueList.at(scaling_index));
        destination.maximum->replace(
            map_index, ecuCalDef->ScalingMaxValueList.at(scaling_index));
        destination.endian->replace(
            map_index, ecuCalDef->ScalingEndianList.at(scaling_index));
        destination.from_byte->replace(
            map_index, ecuCalDef->ScalingFromByteList.at(scaling_index));
        destination.to_byte->replace(
            map_index, ecuCalDef->ScalingToByteList.at(scaling_index));
        destination.format->replace(
            map_index,
            legacyScalingFormat(
                ecuCalDef->ScalingFormatList.at(scaling_index)));
        if (destination.type &&
            ecuCalDef->ScalingStorageTypeList.at(scaling_index) == "bloblist")
        {
            destination.type->replace(map_index, "Selectable");
            destination.selection_names->replace(
                map_index,
                ecuCalDef->ScalingSelectionsNameList.at(scaling_index));
            destination.selection_values->replace(
                map_index,
                ecuCalDef->ScalingSelectionsValueList.at(scaling_index));
        }
    };

    const auto destinations = std::to_array({
        fastecu::definitions::legacy_columns::map_scaling_destination(*ecuCalDef),
        fastecu::definitions::legacy_columns::axis_scaling_destination(*ecuCalDef, true),
        fastecu::definitions::legacy_columns::axis_scaling_destination(*ecuCalDef, false),
    });
    const auto apply_matching_scaling =
        [ecuCalDef, &apply_scaling_columns](
            qsizetype map_index, const auto& destination)
    {
        for (qsizetype scaling_index = 0;
             scaling_index < ecuCalDef->ScalingNameList.size();
             ++scaling_index)
        {
            if (ecuCalDef->ScalingNameList.at(scaling_index) ==
                destination.scaling_name->at(map_index))
            {
                apply_scaling_columns(map_index, scaling_index, destination);
            }
        }
    };
    for (def_map_index = 0; def_map_index < ecuCalDef->NameList.size(); ++def_map_index)
    {
        for (const auto& destination : destinations)
        {
            apply_matching_scaling(def_map_index, destination);
        }
    }
    return ecuCalDef;
}
