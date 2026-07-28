#include "src/backend/definitions/file_actions.h"

// Qt compatibility wrappers over the portable definition service.

#include <algorithm>
#include <set>
#include <string>

namespace
{

std::string ecuflashUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

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
            ecuflashUtf8(
                configValues->ecuflash_definition_files_directory));
    if (!replaced)
    {
        log_definition_error(
            "Unable to build EcuFlash definition catalog",
            replaced.error());
        return configValues;
    }

    for (QString& address : configValues->ecuflash_def_cal_id_addr)
    {
        if (address.startsWith("0x"))
        {
            address.remove(0, 2);
        }
    }
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

    auto catalog =
        build_definition_catalog(
            fastecu::definition::DefinitionFormat::EcuFlash);
    if (!catalog)
    {
        log_definition_error(
            "Unable to read EcuFlash definition " + cal_id,
            catalog.error());
        if (!source.isEmpty() &&
            !definitionFileSystem_.exists(ecuflashUtf8(source)))
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
        fastecu::definition::DefinitionFormat::EcuFlash,
        ecuflashUtf8(cal_id));
    if (!replaced)
    {
        log_definition_error(
            "Unable to read EcuFlash definition " + cal_id,
            replaced.error());
        if (!source.isEmpty() &&
            !definitionFileSystem_.exists(ecuflashUtf8(source)))
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
    for (def_map_index = 0;
         def_map_index < ecuCalDef->NameList.size();
         ++def_map_index)
    {
        for (qsizetype scalingIndex = 0;
             scalingIndex < ecuCalDef->ScalingNameList.size();
             ++scalingIndex)
        {
            if (ecuCalDef->ScalingNameList.at(scalingIndex) ==
                ecuCalDef->MapScalingNameList.at(def_map_index))
            {
                if (ecuCalDef->ScalingStorageTypeList.at(scalingIndex) ==
                    "bloblist")
                {
                    ecuCalDef->TypeList.replace(
                        def_map_index,
                        "Selectable");
                    ecuCalDef->SelectionsNameList.replace(
                        def_map_index,
                        ecuCalDef->ScalingSelectionsNameList.at(
                            scalingIndex));
                    ecuCalDef->SelectionsValueList.replace(
                        def_map_index,
                        ecuCalDef->ScalingSelectionsValueList.at(
                            scalingIndex));
                }
                ecuCalDef->StorageTypeList.replace(
                    def_map_index,
                    ecuCalDef->ScalingStorageTypeList.at(scalingIndex));
                ecuCalDef->UnitsList.replace(
                    def_map_index,
                    ecuCalDef->ScalingUnitsList.at(scalingIndex));
                ecuCalDef->FineIncList.replace(
                    def_map_index,
                    ecuCalDef->ScalingFineIncList.at(scalingIndex));
                ecuCalDef->CoarseIncList.replace(
                    def_map_index,
                    ecuCalDef->ScalingCoarseIncList.at(scalingIndex));
                ecuCalDef->MinValueList.replace(
                    def_map_index,
                    ecuCalDef->ScalingMinValueList.at(scalingIndex));
                ecuCalDef->MaxValueList.replace(
                    def_map_index,
                    ecuCalDef->ScalingMaxValueList.at(scalingIndex));
                ecuCalDef->EndianList.replace(
                    def_map_index,
                    ecuCalDef->ScalingEndianList.at(scalingIndex));
                ecuCalDef->FromByteList.replace(
                    def_map_index,
                    ecuCalDef->ScalingFromByteList.at(scalingIndex));
                ecuCalDef->ToByteList.replace(
                    def_map_index,
                    ecuCalDef->ScalingToByteList.at(scalingIndex));
                ecuCalDef->FormatList.replace(
                    def_map_index,
                    legacyScalingFormat(
                        ecuCalDef->ScalingFormatList.at(scalingIndex)));
            }
            if (ecuCalDef->ScalingNameList.at(scalingIndex) ==
                ecuCalDef->XScaleScalingNameList.at(def_map_index))
            {
                ecuCalDef->XScaleStorageTypeList.replace(
                    def_map_index,
                    ecuCalDef->ScalingStorageTypeList.at(scalingIndex));
                ecuCalDef->XScaleUnitsList.replace(
                    def_map_index,
                    ecuCalDef->ScalingUnitsList.at(scalingIndex));
                ecuCalDef->XScaleFineIncList.replace(
                    def_map_index,
                    ecuCalDef->ScalingFineIncList.at(scalingIndex));
                ecuCalDef->XScaleCoarseIncList.replace(
                    def_map_index,
                    ecuCalDef->ScalingCoarseIncList.at(scalingIndex));
                ecuCalDef->XScaleMinValueList.replace(
                    def_map_index,
                    ecuCalDef->ScalingMinValueList.at(scalingIndex));
                ecuCalDef->XScaleMaxValueList.replace(
                    def_map_index,
                    ecuCalDef->ScalingMaxValueList.at(scalingIndex));
                ecuCalDef->XScaleEndianList.replace(
                    def_map_index,
                    ecuCalDef->ScalingEndianList.at(scalingIndex));
                ecuCalDef->XScaleFromByteList.replace(
                    def_map_index,
                    ecuCalDef->ScalingFromByteList.at(scalingIndex));
                ecuCalDef->XScaleToByteList.replace(
                    def_map_index,
                    ecuCalDef->ScalingToByteList.at(scalingIndex));
                ecuCalDef->XScaleFormatList.replace(
                    def_map_index,
                    legacyScalingFormat(
                        ecuCalDef->ScalingFormatList.at(scalingIndex)));
            }
            if (ecuCalDef->ScalingNameList.at(scalingIndex) ==
                ecuCalDef->YScaleScalingNameList.at(def_map_index))
            {
                ecuCalDef->YScaleStorageTypeList.replace(
                    def_map_index,
                    ecuCalDef->ScalingStorageTypeList.at(scalingIndex));
                ecuCalDef->YScaleUnitsList.replace(
                    def_map_index,
                    ecuCalDef->ScalingUnitsList.at(scalingIndex));
                ecuCalDef->YScaleFineIncList.replace(
                    def_map_index,
                    ecuCalDef->ScalingFineIncList.at(scalingIndex));
                ecuCalDef->YScaleCoarseIncList.replace(
                    def_map_index,
                    ecuCalDef->ScalingCoarseIncList.at(scalingIndex));
                ecuCalDef->YScaleMinValueList.replace(
                    def_map_index,
                    ecuCalDef->ScalingMinValueList.at(scalingIndex));
                ecuCalDef->YScaleMaxValueList.replace(
                    def_map_index,
                    ecuCalDef->ScalingMaxValueList.at(scalingIndex));
                ecuCalDef->YScaleEndianList.replace(
                    def_map_index,
                    ecuCalDef->ScalingEndianList.at(scalingIndex));
                ecuCalDef->YScaleFromByteList.replace(
                    def_map_index,
                    ecuCalDef->ScalingFromByteList.at(scalingIndex));
                ecuCalDef->YScaleToByteList.replace(
                    def_map_index,
                    ecuCalDef->ScalingToByteList.at(scalingIndex));
                ecuCalDef->YScaleFormatList.replace(
                    def_map_index,
                    legacyScalingFormat(
                        ecuCalDef->ScalingFormatList.at(scalingIndex)));
            }
        }
    }
    return ecuCalDef;
}
